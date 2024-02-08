#ifndef __XRDPFC_FILEINSTRUCTPURGEPLG_HH__
#define __XRDPFC_FILEINSTRUCTPURGEPLG_HH__

#include "XrdPfcDirPurge.hh"
#include "XrdPfcDirState.hh"

class XrdPfcDirPurgeFileCfg : public XrdPfc::DirPurge
{
public:
    {
        DirInfo d1;
        d1.path = "/test/a_LEVEL_1/";
        d1.nBytesQuota = 30 * 1024LL * 1024LL * 1024LL;
        m_list.push_back(d1);

        DirInfo d2;
        d2.path = "/test/b_LEVEL_1/";
        d2.nBytesQuota = 10 * 1024LL * 1024LL * 1024LL;
        m_list.push_back(d2);
    }

    //----------------------------------------------------------------------------
    //! Provide bytes to erase from dir quota listed in a text file
    //----------------------------------------------------------------------------
    virtual long long GetBytesToRecover(XrdPfc::DirState *ds)
    {
        // setup disuusage for each dir path
        InitDirStatesForLocalPaths(ds);

        long long totalToRemove = 0;
        // get bytes to remove
        for (list_i it = m_list.begin(); it != m_list.end(); ++it)
        {
            long long cv = it->dirState->get_usage() - it->nBytesQuota;
            if (cv > 0)
                it->nBytesToRecover = cv;
            else
                it->nBytesToRecover = 0;

            totalToRemove += it->nBytesToRecover; 
        }

        return totalToRemove;
    }
};

#endif
