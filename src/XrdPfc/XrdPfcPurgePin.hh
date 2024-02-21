#ifndef __XRDPFC_PURGEPLG_HH__
#define __XRDPFC_PURGEPLG_HH__

#include <string>
#include <vector>

namespace XrdPfc
{
class DirState;

//----------------------------------------------------------------------------
//! Base class for reguesting directory space to obtain.
//----------------------------------------------------------------------------
class PurgePin
{
public:
   struct DirInfo
   {
      std::string path;
      long long nBytesQuota{0};
      DirState *dirState{nullptr}; // currently cached and shared within the purge thread
      long long nBytesToRecover{0};
   };

   typedef std::vector<DirInfo> list_t;
   typedef list_t::iterator list_i;

protected:
   list_t m_list;

public:
   virtual ~PurgePin() {}

   //---------------------------------------------------------------------
   //! Provide erase information from directory statistics
   //!
   //! @param & XrdPfcDirState
   //!
   //! @return total number of bytes
   //---------------------------------------------------------------------
   virtual long long GetBytesToRecover(DirState *) = 0;

   //------------------------------------------------------------------------------
   //! Parse configuration arguments.
   //!
   //! @param params configuration parameters
   //!
   //! @return status of configuration
   //------------------------------------------------------------------------------
   virtual bool ConfigPurgePin(const char* params)  // ?? AMT should this be abstract
   {
      (void) params;
      return true;
   }

   //-----------------------------------------------
   //!
   //!  Get quotas for the given paths. Used in the XrdPfc:Cache::Purge() thread.
   //!
   //------------------------------------------------------------------------------
   list_t &refDirInfos() { return m_list; }
};
}

#endif
