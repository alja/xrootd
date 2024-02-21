#include "XrdPfc.hh"
#include "XrdPfcDirPurge.hh"
#include "XrdPfcDirState.hh"

#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucUtils.hh"
#include "XrdOuc/XrdOucStream.hh"
#include "XrdOuc/XrdOuca2x.hh"

#include <fcntl.h>

class XrdPfcDirPurgeFileCfg : public XrdPfc::DirPurge
{
public:
    XrdPfcDirPurgeFileCfg () {}

    //----------------------------------------------------------------------------
    //! Set directory statistics
    //----------------------------------------------------------------------------
    void InitDirStatesForLocalPaths(XrdPfc::DirState *rootDS)
    {
        for (list_i it = m_list.begin(); it != m_list.end(); ++it)
        {
            it->dirState = rootDS->find_path(it->path, Cache::Conf().m_dirStatsStoreDepth, false, false);
        }
    }

    //----------------------------------------------------------------------------
    //! Provide bytes to erase from dir quota listed in a text file
    //----------------------------------------------------------------------------
    virtual long long GetBytesToRecover(XrdPfc::DirState *ds)
    {
        // setup diskusage for each dir path
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

    //----------------------------------------------------------------------------
    //! Provide bytes to erase from dir quota listed in a text file
    //----------------------------------------------------------------------------
    virtual bool ConfigDirPurge(const char *parms)
    {
        XrdSysError *log = Cache::GetInstance().GetLog();
         
        // retrive configuration file name
        if (!parms || !parms[0] || (strlen(parms) == 0))
        {
            log->Emsg("ConfigDecision", "Quota file not specified.");
            return false;
        }
        log->Emsg("ConfigDecision", "Using directory list", parms);

        //  parse the file to get directory quotas
        const char* config_filename = parms;
        const char *theINS = getenv("XRDINSTANCE");
        XrdOucEnv myEnv;
        XrdOucStream Config(log, theINS, &myEnv, "=====> PurgeFileCfg ");

        int fd;
        if ((fd = open(config_filename, O_RDONLY, 0)) < 0)
        {
            log->Emsg("Config() can't open configuration file ", config_filename);
        }

        Config.Attach(fd);
        static const char *cvec[] = {"*** pfc purge plugin :", 0}; // ?? AMT where is this used
        Config.Capture(cvec);

        char *var;
        while ((var = Config.GetMyFirstWord()))
        {
            std::string dirpath = var;
            const char* val;

            if (!(val = Config.GetWord()))
            {
                log->Emsg("PurgeFileCfg", "quota not specified");
                continue;
            }

            std::string tmpc = val;
            long long quota = 0;
            if (::isalpha(*(tmpc.rbegin())))
            {
                if (XrdOuca2x::a2sz(*log, "Error getting quota", tmpc.c_str(),&quota))
                {
                    continue;
                }
            }
            else
            {
                if (XrdOuca2x::a2ll(*log, "Error getting quota", tmpc.c_str(),&quota))
                {
                    continue;
                }
            }

            DirInfo d;
            d.path = dirpath;
            d.nBytesQuota = quota;
            m_list.push_back(d);
        }

        return true;
    }
};

/******************************************************************************/
/*                          XrdPfcGetDirPurge                                 */
/******************************************************************************/

// Return a purge object to use.
extern "C"
{
    XrdPfc::DirPurge *XrdPfcGetDirPurge(XrdSysError &)
    {
        return new XrdPfcDirPurgeFileCfg();
    }
}
