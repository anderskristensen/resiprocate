#include "BridgeMixer.hxx"
#include "UserAgentSubsystem.hxx"
#include "MediaResourceParticipant.hxx"
#include "ConversationManager.hxx"
#include "Conversation.hxx"
#include "UserAgent.hxx"

#include <rutil/Log.hxx>
#include <rutil/Logger.hxx>
#include <resip/stack/ExtensionParameter.hxx>
#include <rutil/WinLeakCheck.hxx>

// sipX includes
#include "mp/dtmflib.h"
#include "mp/MprFromFile.h"
#include "mp/MpStreamPlayer.h"

using namespace useragent;
using namespace resip;
using namespace std;

#define RESIPROCATE_SUBSYSTEM UserAgentSubsystem::USERAGENT

static const resip::ExtensionParameter p_localonly("local-only");
static const resip::ExtensionParameter p_remoteonly("remote-only");
static const resip::ExtensionParameter p_repeat("repeat");
static const resip::ExtensionParameter p_prefetch("prefetch");

// Url schemes
static const Data toneScheme("tone");
static const Data fileScheme("file");
static const Data cacheScheme("cache");
static const Data httpScheme("http");
static const Data httpsScheme("https");

// Special Tones
static const Data dialtoneTone("dialtone");
static const Data busyTone("busy");
static const Data ringbackTone("ringback");
static const Data ringTone("ring");
static const Data fastbusyTone("fastbusy");
static const Data backspaceTone("backspace");
static const Data callwaitingTone("callwaiting");
static const Data holdingTone("holding");
static const Data loudfastbusyTone("loudfastbusy");

namespace useragent
{
// Used to destroy a media participant after a timer expires
class MediaResourceParticipantDestroyer : public DumCommand
{
   public:
      MediaResourceParticipantDestroyer(ConversationManager& conversationManager, ConversationManager::ParticipantHandle participantHandle) :
         mConversationManager(conversationManager), mParticipantHandle(participantHandle) {}
      MediaResourceParticipantDestroyer(const MediaResourceParticipantDestroyer& rhs) :
         mConversationManager(rhs.mConversationManager), mParticipantHandle(rhs.mParticipantHandle) {}
      ~MediaResourceParticipantDestroyer() {}

      void executeCommand() { mConversationManager.destroyParticipant(mParticipantHandle); }

      Message* clone() const { return new MediaResourceParticipantDestroyer(*this); }
      std::ostream& encode(std::ostream& strm) const { strm << "MediaResourceParticipantDestroyer: partHandle=" << mParticipantHandle; return strm; }
      std::ostream& encodeBrief(std::ostream& strm) const { return encode(strm); }
      
   private:
      ConversationManager& mConversationManager;
      ConversationManager::ParticipantHandle mParticipantHandle;
};

// Used to delete a resource, from a sipX thread
class MediaResourceParticipantDeleterCmd : public DumCommand
{
   public:
      MediaResourceParticipantDeleterCmd(ConversationManager& conversationManager, ConversationManager::ParticipantHandle participantHandle) :
         mConversationManager(conversationManager), mParticipantHandle(participantHandle) {}
      ~MediaResourceParticipantDeleterCmd() {}

      void executeCommand() { Participant* participant = mConversationManager.getParticipant(mParticipantHandle); if(participant) delete participant; }

      Message* clone() const { assert(0); return 0; }
      std::ostream& encode(std::ostream& strm) const { strm << "MediaResourceParticipantDeleterCmd: partHandle=" << mParticipantHandle; return strm; }
      std::ostream& encodeBrief(std::ostream& strm) const { return encode(strm); }
      
   private:
      ConversationManager& mConversationManager;
      ConversationManager::ParticipantHandle mParticipantHandle;
};
}

MediaResourceParticipant::MediaResourceParticipant(ConversationManager::ParticipantHandle partHandle,
                                                   ConversationManager& conversationManager,
                                                   Uri& mediaUrl)
: Participant(partHandle, conversationManager),
  mMediaUrl(mediaUrl),
  mStreamPlayer(0),
  mLocalOnly(false),
  mRemoteOnly(false),
  mRepeat(false),
  mPrefetch(false),
  mDurationMs(0),
  mPlaying(false),
  mDestroying(false)
{
   InfoLog(<< "MediaResourceParticipant created, handle=" << mHandle << " url=" << mMediaUrl);
   mResourceType = Invalid;  // default
   try
   {
      if(isEqualNoCase(mMediaUrl.scheme(), toneScheme))
      {
         mResourceType = Tone;
      }
      else if(isEqualNoCase(mMediaUrl.scheme(), fileScheme))
      {
         mResourceType = File;
      }
      else if(isEqualNoCase(mMediaUrl.scheme(), cacheScheme))
      {
         mResourceType = Cache;
      }
      else if(isEqualNoCase(mMediaUrl.scheme(), httpScheme))
      {
         mResourceType = Http;
      }
      else if(isEqualNoCase(mMediaUrl.scheme(), httpsScheme))
      {
         mResourceType = Https;
      }
   }
   catch(BaseException &e)
   {
      WarningLog(<< "MediaResourceParticipant::MediaResourceParticipant exception: " << e);
   }
   catch(...)
   {
      WarningLog(<< "MediaResourceParticipant::MediaResourceParticipant unknown exception");
   }
}

MediaResourceParticipant::~MediaResourceParticipant()
{
   // Destroy stream player (if created)
   if(mStreamPlayer)
   {
      mStreamPlayer->removeListener(this);
      mStreamPlayer->destroy();
   }

   // unregister from Conversations
   // Note:  ideally this functionality would exist in Participant Base class - but dynamic_cast required in unregisterParticipant will not work
   ConversationMap::iterator it;
   for(it = mConversations.begin(); it != mConversations.end(); it++)
   {
      it->second->unregisterParticipant(this);
   }
   mConversations.clear();

   InfoLog(<< "MediaResourceParticipant destroyed, handle=" << mHandle << " url=" << mMediaUrl);
}

void 
MediaResourceParticipant::startPlay()
{
   assert(!mPlaying);
   try
   {
      InfoLog(<< "MediaResourceParticipant playing, handle=" << mHandle << " url=" << mMediaUrl);

      // Common processing
      if(mMediaUrl.exists(p_localonly))
      {
         mLocalOnly = true;
         mMediaUrl.remove(p_localonly);
      }
      if(mMediaUrl.exists(p_remoteonly))
      {
         mRemoteOnly = true;
         mMediaUrl.remove(p_remoteonly);
      }
      if(mMediaUrl.exists(p_duration))
      {
         mDurationMs = mMediaUrl.param(p_duration);
         mMediaUrl.remove(p_duration);
      }
      if(mMediaUrl.exists(p_repeat))
      {
         mRepeat = true;
         mMediaUrl.remove(p_repeat);
      }
      if(mMediaUrl.exists(p_prefetch))
      {
         mPrefetch = true;
         mMediaUrl.remove(p_prefetch);
      }

      switch(mResourceType)
      {
      case Tone:
      {
         int toneid;
         if(mMediaUrl.host().size() == 1)
         {
            toneid = mMediaUrl.host().at(0);
         }
         else
         {
            if(isEqualNoCase(mMediaUrl.host(), dialtoneTone)) toneid = DTMF_TONE_DIALTONE;
            else if(isEqualNoCase(mMediaUrl.host(), busyTone)) toneid = DTMF_TONE_BUSY;
            else if(isEqualNoCase(mMediaUrl.host(), ringbackTone)) toneid = DTMF_TONE_RINGBACK;
            else if(isEqualNoCase(mMediaUrl.host(), ringTone)) toneid = DTMF_TONE_RINGTONE;
            else if(isEqualNoCase(mMediaUrl.host(), fastbusyTone)) toneid = DTMF_TONE_CALLFAILED;
            else if(isEqualNoCase(mMediaUrl.host(), backspaceTone)) toneid = DTMF_TONE_BACKSPACE;
            else if(isEqualNoCase(mMediaUrl.host(), callwaitingTone)) toneid = DTMF_TONE_CALLWAITING;
            else if(isEqualNoCase(mMediaUrl.host(), holdingTone)) toneid = DTMF_TONE_CALLHELD;
            else if(isEqualNoCase(mMediaUrl.host(), loudfastbusyTone)) toneid = DTMF_TONE_LOUD_FAST_BUSY;
            else
            {
               WarningLog(<< "MediaResourceParticipant::startPlay invalid tone identifier: " << mMediaUrl.host());
               return;
            }
         }

         OsStatus status = mConversationManager.getMediaInterface()->startTone(toneid, mRemoteOnly ? FALSE : TRUE /* local */, mLocalOnly ? FALSE : TRUE /* remote */);
         if(status == OS_SUCCESS)
         {
            mPlaying = true;
         }
         else
         {
            WarningLog(<< "MediaResourceParticipant::startPlay error calling startTone: " << status);
         }
      }
      break;
      case File:
      {
         Data filepath = mMediaUrl.host().urlDecoded();
         if(filepath.size() > 3 && filepath.substr(0, 3) == Data("///")) filepath = filepath.substr(2);
         else if(filepath.size() > 2 && filepath.substr(0, 2) == Data("//")) filepath = filepath.substr(1);
         
         filepath.replace("|", ":");  // For Windows filepath processing - convert | to :

         InfoLog(<< "MediaResourceParticipant playing, handle=" << mHandle << " filepath=" << filepath);

         OsStatus status = mConversationManager.getMediaInterface()->playAudio(filepath.c_str(), 
                                                                               mRepeat ? TRUE: FALSE /* repeast? */,
                                                                               mRemoteOnly ? FALSE : TRUE /* local */, 
                                                                               mLocalOnly ? FALSE : TRUE /* remote */,
                                                                               FALSE /* mixWithMic */,
                                                                               100 /* downScaling */,
                                                                               this);
         if(status == OS_SUCCESS)
         {
            mPlaying = true;
         }
         else
         {
            WarningLog(<< "MediaResourceParticipant::startPlay error calling playAudio: " << status);
         }
      }
      break;
      case Cache:
      {
         InfoLog(<< "MediaResourceParticipant playing, handle=" << mHandle << " cacheKey=" << mMediaUrl.host());

         Data *buffer;
         int type;
         if(mConversationManager.mMediaResourceCache.getFromCache(mMediaUrl.host(), &buffer, &type))
         {
            OsStatus status = mConversationManager.getMediaInterface()->playBuffer((char*)buffer->data(),
                                                                                   buffer->size(), 
                                                                                   8000, /* rate */
                                                                                   type, 
                                                                                   mRepeat ? TRUE: FALSE /* repeast? */,
                                                                                   mRemoteOnly ? FALSE : TRUE /* local */, 
                                                                                   mLocalOnly ? FALSE : TRUE /* remote */,
                                                                                   NULL /* OsProtectedEvent */,
                                                                                   FALSE /* mixWithMic */,
                                                                                   100 /* downScaling */,
                                                                                   this);
            if(status == OS_SUCCESS)
            {
               mPlaying = true;
            }
            else
            {
               WarningLog(<< "MediaResourceParticipant::startPlay error calling playAudio: " << status);
            }
         }
         else
         {
            WarningLog(<< "MediaResourceParticipant::startPlay media not found in cache, key: " << mMediaUrl.host());
         }
      }
      break;
      case Http:
      case Https:
      {
         int flags;

         if(mLocalOnly)
         {
            flags = STREAM_SOUND_LOCAL;
         }
         else if(mRemoteOnly)
         {
            flags = STREAM_SOUND_REMOTE;
         }
         else 
         {
            flags = STREAM_SOUND_LOCAL | STREAM_SOUND_REMOTE;
         }
         OsStatus status = mConversationManager.getMediaInterface()->createPlayer(&mStreamPlayer, Data::from(mMediaUrl).c_str(), flags);
         if(status == OS_SUCCESS)
         {
            mStreamPlayer->addListener(this);
            status = mStreamPlayer->realize(FALSE /* block? */);
            if(status != OS_SUCCESS)
            {
               WarningLog(<< "MediaResourceParticipant::startPlay error calling StreamPlayer::realize: " << status);
            }
            else
            {
               mPlaying = true;
            }
         }
         else
         {
            WarningLog(<< "MediaResourceParticipant::startPlay error calling createPlayer: " << status);
         }
      }
      break;
      case Invalid:
         WarningLog(<< "MediaResourceParticipant::startPlay invalid resource type: " << mMediaUrl.scheme());
         break;
      }
   }
   catch(BaseException &e)
   {
      WarningLog(<< "MediaResourceParticipant::startPlay exception: " << e);
   }
   catch(...)
   {
      WarningLog(<< "MediaResourceParticipant::startPlay unknown exception");
   }

   if(mPlaying)  // If play started successfully
   {
      if(mDurationMs > 0)
      {
         // Start timer to destroy media resource participant automatically
         MediaResourceParticipantDestroyer destroyer(mConversationManager, mHandle);
         mConversationManager.getUserAgent()->post(destroyer, mDurationMs);
      }
   }
   else
   {
      delete this;
   }
}

int 
MediaResourceParticipant::getConnectionPortOnBridge()
{
   int connectionPort = -1;
   switch(mResourceType)
   {
   case Tone:
      connectionPort = DEFAULT_TONE_PLAYER_BRIDGE_CONNECTION_PORT;
      break;
   case File:
   case Cache:
   case Http:
   case Https:
      connectionPort = DEFAULT_FILE_PLAYER_BRIDGE_CONNECTION_PORT;
      break;
   case Invalid:
      WarningLog(<< "MediaResourceParticipant::getConnectionPortOnBridge invalid resource type: " << mResourceType);
      break;
   }
   return connectionPort;
}

void
MediaResourceParticipant::destroyParticipant()
{
   bool deleteNow = true;

   if(mDestroying) return;
   mDestroying = true;

   if(mPlaying)
   {
      switch(mResourceType)
      {
      case Tone:
         {
            OsStatus status = mConversationManager.getMediaInterface()->stopTone();
            if(status != OS_SUCCESS)
            {
               WarningLog(<< "MediaResourceParticipant::destroyParticipant error calling stopTone: " << status);
            }
         }
         break;
      case File:
      case Cache:
         {
            OsStatus status = mConversationManager.getMediaInterface()->stopAudio();
            if(status != OS_SUCCESS)
            {
               WarningLog(<< "MediaResourceParticipant::destroyParticipant error calling stopAudio: " << status);
            }
            else
            {
               deleteNow = false;  // Wait for play finished event to come in
            }
         }
         break;
      case Http:
      case Https:
         {
            mRepeat = false;  // Required so that player will not just repeat on stopped event
            OsStatus status = mStreamPlayer->stop();
            if(status != OS_SUCCESS)
            {
               WarningLog(<< "MediaResourceParticipant::destroyParticipant error calling StreamPlayer::stop: " << status);
            }
            else
            {
               deleteNow = false;  // Wait for play finished event to come in
            }
         }
         break;
      case Invalid:
         WarningLog(<< "MediaResourceParticipant::destroyParticipant invalid resource type: " << mResourceType);
         break;
      }
   }
   if(deleteNow) delete this;
}

OsStatus 
MediaResourceParticipant::signal(const int eventData)
{
   switch(eventData)
   {
   case MprFromFile::PLAY_FINISHED:
      InfoLog(<< "MediaResourceParticipant::signal eventData: PLAY_FINISHED handle=" << mHandle);
      {
         MediaResourceParticipantDeleterCmd* cmd = new MediaResourceParticipantDeleterCmd(mConversationManager, mHandle);
         mConversationManager.getUserAgent()->getDialogUsageManager().post(cmd);
      }
      break;
   case MprFromFile::PLAY_STOPPED:
      InfoLog(<< "MediaResourceParticipant::signal eventData: PLAY_STOPPED handle=" << mHandle);
      mPlaying = false;
      break;
   case MprFromFile::PLAYING:
      InfoLog(<< "MediaResourceParticipant::signal eventData: PLAYING handle=" << mHandle);
      mPlaying = true;
      break;
   case MprFromFile::READ_ERROR:
      InfoLog(<< "MediaResourceParticipant::signal eventData: READ_ERROR handle=" << mHandle);
      {
         MediaResourceParticipantDeleterCmd* cmd = new MediaResourceParticipantDeleterCmd(mConversationManager, mHandle);
         mConversationManager.getUserAgent()->getDialogUsageManager().post(cmd);
      }
      break;
   case MprFromFile::PLAY_IDLE:
      InfoLog(<< "MediaResourceParticipant::signal eventData: PLAY_IDLE handle=" << mHandle);
      mPlaying = false;
      // ?SLG? Should we do anything here?
      break;
   case MprFromFile::INVALID_SETUP:
      InfoLog(<< "MediaResourceParticipant::signal eventData: INVALID_SETUP handle=" << mHandle);
      {
         MediaResourceParticipantDeleterCmd* cmd = new MediaResourceParticipantDeleterCmd(mConversationManager, mHandle);
         mConversationManager.getUserAgent()->getDialogUsageManager().post(cmd);
      }
      break;
   default:
      WarningLog(<< "MediaResourceParticipant::signal eventData unrecognized: " << eventData << " handle=" << mHandle);
      break;
   }

   return OS_SUCCESS;
}

void 
MediaResourceParticipant::playerRealized(MpPlayerEvent& event)
{
   InfoLog(<< "MediaResourceParticipant::playerRealized: handle=" << mHandle);
   if(mPrefetch)
   {
      OsStatus status = mStreamPlayer->prefetch(FALSE);
      if(status != OS_SUCCESS)
      {
         WarningLog(<< "MediaResourceParticipant::playerRealized error calling StreamPlayer::prefetch: " << status);
         MediaResourceParticipantDeleterCmd* cmd = new MediaResourceParticipantDeleterCmd(mConversationManager, mHandle);
         mConversationManager.getUserAgent()->getDialogUsageManager().post(cmd);
      }
   }
   else
   {
      OsStatus status = mStreamPlayer->play(FALSE /*block?*/);
      if(status != OS_SUCCESS)
      {
         WarningLog(<< "MediaResourceParticipant::playerRealized error calling StreamPlayer::play: " << status);
         MediaResourceParticipantDeleterCmd* cmd = new MediaResourceParticipantDeleterCmd(mConversationManager, mHandle);
         mConversationManager.getUserAgent()->getDialogUsageManager().post(cmd);
      }
   }
}

void 
MediaResourceParticipant::playerPrefetched(MpPlayerEvent& event)
{
   InfoLog(<< "MediaResourceParticipant::playerPrefetched: handle=" << mHandle);
   OsStatus status = mStreamPlayer->play(FALSE/*block?*/);
   if(status != OS_SUCCESS)
   {
      WarningLog(<< "MediaResourceParticipant::playerPrefetched error calling StreamPlayer::play: " << status);
       MediaResourceParticipantDeleterCmd* cmd = new MediaResourceParticipantDeleterCmd(mConversationManager, mHandle);
       mConversationManager.getUserAgent()->getDialogUsageManager().post(cmd);
   }
}

void 
MediaResourceParticipant::playerPlaying(MpPlayerEvent& event)
{
   InfoLog(<< "MediaResourceParticipant::playerPlaying: handle=" << mHandle);
}

void 
MediaResourceParticipant::playerPaused(MpPlayerEvent& event)
{
   InfoLog(<< "MediaResourceParticipant::playerPaused: handle=" << mHandle);
}

void 
MediaResourceParticipant::playerStopped(MpPlayerEvent& event)
{
   InfoLog(<< "MediaResourceParticipant::playerStopped: handle=" << mHandle);
   // We get this event when playing is completed
   if(mRepeat)
   {
      OsStatus status = mStreamPlayer->rewind(FALSE/*block?*/);   // Generate playerPrefetched event
      if(status != OS_SUCCESS)
      {
         WarningLog(<< "MediaResourceParticipant::playerStopped error calling StreamPlayer::rewind: " << status);
         MediaResourceParticipantDeleterCmd* cmd = new MediaResourceParticipantDeleterCmd(mConversationManager, mHandle);
         mConversationManager.getUserAgent()->getDialogUsageManager().post(cmd);
      }
   }
   else
   {
      MediaResourceParticipantDeleterCmd* cmd = new MediaResourceParticipantDeleterCmd(mConversationManager, mHandle);
      mConversationManager.getUserAgent()->getDialogUsageManager().post(cmd);
   }
}
 
void 
MediaResourceParticipant::playerFailed(MpPlayerEvent& event)
{
   InfoLog(<< "MediaResourceParticipant::playerFailed: handle=" << mHandle);
   MediaResourceParticipantDeleterCmd* cmd = new MediaResourceParticipantDeleterCmd(mConversationManager, mHandle);
   mConversationManager.getUserAgent()->getDialogUsageManager().post(cmd);
}


/* ====================================================================

 Original contribution Copyright (C) 2008 Plantronics, Inc.
 Provided under the terms of the Vovida Software License, Version 2.0.

 The Vovida Software License, Version 2.0 
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
 
 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution. 
 
 THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 DAMAGE.

 ==================================================================== */
