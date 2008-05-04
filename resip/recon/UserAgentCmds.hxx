#if !defined(ConversationManagerCmds_hxx)
#define ConversationManagerCmds_hxx

#include <resip/dum/DumCommand.hxx>

#include "UserAgent.hxx"

namespace useragent
{

/**
  The classes defined here are used to pass commands from the
  application thread to the UserAgent thread (process loop).  
  This ensures thread safety of the UserAgent methods that are
  available to an application.

  Author: Scott Godin (sgodin AT SipSpectrum DOT com)
*/

class UserAgentShutdownCmd  : public resip::DumCommand
{
   public:  
      UserAgentShutdownCmd(UserAgent* userAgent)
         : mUserAgent(userAgent) {}
      virtual void executeCommand()
      {
         mUserAgent->shutdownImpl();
      }
      resip::Message* clone() const { assert(0); return 0; }
      std::ostream& encode(std::ostream& strm) const { strm << " UserAgentShutdownCmd: "; return strm; }
      std::ostream& encodeBrief(std::ostream& strm) const { return encode(strm); }
   private:
      UserAgent* mUserAgent;
};

class AddConversationProfileCmd  : public resip::DumCommand
{
   public:  
      AddConversationProfileCmd(UserAgent* userAgent,
                                UserAgent::ConversationProfileHandle handle,
                                resip::SharedPtr<ConversationProfile> conversationProfile,
                                bool defaultOutgoing)
         : mUserAgent(userAgent),
           mHandle(handle),
           mConversationProfile(conversationProfile),
           mDefaultOutgoing(defaultOutgoing) {}
      virtual void executeCommand()
      {
         mUserAgent->addConversationProfileImpl(mHandle, mConversationProfile, mDefaultOutgoing);
      }
      resip::Message* clone() const { assert(0); return 0; }
      std::ostream& encode(std::ostream& strm) const { strm << " AddConversationProfileCmd: "; return strm; }
      std::ostream& encodeBrief(std::ostream& strm) const { return encode(strm); }
   private:
      UserAgent* mUserAgent;
      UserAgent::ConversationProfileHandle mHandle;
      resip::SharedPtr<ConversationProfile> mConversationProfile;
      bool mDefaultOutgoing;
};

class SetDefaultOutgoingConversationProfileCmd  : public resip::DumCommand
{
   public:  
      SetDefaultOutgoingConversationProfileCmd(UserAgent* userAgent,
                                               UserAgent::ConversationProfileHandle handle)
         : mUserAgent(userAgent),
           mHandle(handle) {}
      virtual void executeCommand()
      {
         mUserAgent->setDefaultOutgoingConversationProfileImpl(mHandle);
      }
      resip::Message* clone() const { assert(0); return 0; }
      std::ostream& encode(std::ostream& strm) const { strm << " SetDefaultOutgoingConversationProfileCmd: "; return strm; }
      std::ostream& encodeBrief(std::ostream& strm) const { return encode(strm); }
   private:
      UserAgent* mUserAgent;
      UserAgent::ConversationProfileHandle mHandle;
};

class DestroyConversationProfileCmd  : public resip::DumCommand
{
   public:  
      DestroyConversationProfileCmd(UserAgent* userAgent,
                                    UserAgent::ConversationProfileHandle handle)
         : mUserAgent(userAgent),
           mHandle(handle) {}
      virtual void executeCommand()
      {
         mUserAgent->destroyConversationProfileImpl(mHandle);
      }
      resip::Message* clone() const { assert(0); return 0; }
      std::ostream& encode(std::ostream& strm) const { strm << " DestroyConversationProfileCmd: "; return strm; }
      std::ostream& encodeBrief(std::ostream& strm) const { return encode(strm); }
   private:
      UserAgent* mUserAgent;
      UserAgent::ConversationProfileHandle mHandle;
};

class UserAgentTimeout : public resip::DumCommand
{
   public:
      UserAgentTimeout(UserAgent& userAgent, unsigned int timerId, unsigned int duration, unsigned int seqNumber) :
         mUserAgent(userAgent), mTimerId(timerId), mDuration(duration), mSeqNumber(seqNumber) {}
      UserAgentTimeout(const UserAgentTimeout& rhs) :
         mUserAgent(rhs.mUserAgent), mTimerId(rhs.mTimerId), mDuration(rhs.mDuration), mSeqNumber(rhs.mSeqNumber) {}
      ~UserAgentTimeout() {}

      void executeCommand() { mUserAgent.onApplicationTimer(mTimerId, mDuration, mSeqNumber); }

      resip::Message* clone() const { return new UserAgentTimeout(*this); }
      std::ostream& encode(std::ostream& strm) const { strm << "UserAgentTimeout: id=" << mTimerId << ", duration=" << mDuration << ", seq=" << mSeqNumber; return strm; }
      std::ostream& encodeBrief(std::ostream& strm) const { return encode(strm); }

      unsigned int id() const { return mTimerId; }
      unsigned int seqNumber() const { return mSeqNumber; }
      unsigned int duration() const { return mDuration; }
      
   private:
      UserAgent& mUserAgent;
      unsigned int mTimerId;
      unsigned int mDuration;
      unsigned int mSeqNumber;
};

class CreateSubscriptionCmd  : public resip::DumCommand
{
   public:  
      CreateSubscriptionCmd(UserAgent* userAgent,
                            UserAgent::SubscriptionHandle handle,
                            const resip::Data& eventType, 
                            const resip::NameAddr& target, 
                            unsigned int subscriptionTime, 
                            const resip::Mime& mimeType)
         : mUserAgent(userAgent),
           mHandle(handle),
           mEventType(eventType),
           mTarget(target),
           mSubscriptionTime(subscriptionTime),
           mMimeType(mimeType) {}
      virtual void executeCommand()
      {
         mUserAgent->createSubscriptionImpl(mHandle, mEventType, mTarget, mSubscriptionTime, mMimeType);
      }
      resip::Message* clone() const { assert(0); return 0; }
      std::ostream& encode(std::ostream& strm) const { strm << " CreateSubscriptionCmd: "; return strm; }
      std::ostream& encodeBrief(std::ostream& strm) const { return encode(strm); }
   private:
      UserAgent* mUserAgent;
      UserAgent::SubscriptionHandle mHandle;
      resip::Data mEventType;
      resip::NameAddr mTarget;
      unsigned int mSubscriptionTime;
      resip::Mime mMimeType;
};

class DestroySubscriptionCmd  : public resip::DumCommand
{
   public:  
      DestroySubscriptionCmd(UserAgent* userAgent,
                             UserAgent::SubscriptionHandle handle)
         : mUserAgent(userAgent),
           mHandle(handle) {}
      virtual void executeCommand()
      {
         mUserAgent->destroySubscriptionImpl(mHandle);
      }
      resip::Message* clone() const { assert(0); return 0; }
      std::ostream& encode(std::ostream& strm) const { strm << " DestroySubscriptionCmd: "; return strm; }
      std::ostream& encodeBrief(std::ostream& strm) const { return encode(strm); }
   private:
      UserAgent* mUserAgent;
      UserAgent::SubscriptionHandle mHandle;
};

}

#endif


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
