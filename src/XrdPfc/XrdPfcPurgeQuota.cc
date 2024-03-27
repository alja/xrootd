#include "XrdPfc.hh"
#include "XrdPfcPurgePin.hh"
#include "XrdPfcDirState.hh"

#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucUtils.hh"
#include "XrdOuc/XrdOucStream.hh"
#include "XrdOuc/XrdOuca2x.hh"

#include <fcntl.h>

class XrdPfcPurgeQuota : public XrdPfc::PurgePin
{
public:
    XrdPfcPurgeQuota () {}

    //----------------------------------------------------------------------------
    //! Set directory statistics
    //----------------------------------------------------------------------------
    void InitDirStatesForLocalPaths(XrdPfc::DirState *rootDS)
    {
        for (list_i it = m_list.begin(); it != m_list.end(); ++it)
        {
            it->dirState = rootDS->find_path(it->path, XrdPfc::Cache::Conf().m_dirStatsStoreDepth, false, false);
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
            // XXXXXX here we should have another mechanism. and probably add up here + subdirs
            long long cv = it->dirState->m_recursive_subdir_usage.m_bytes_on_disk - it->nBytesQuota;
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
    virtual bool ConfigPurgePin(const char *parms)
    {
        XrdSysError *log = XrdPfc::Cache::GetInstance().GetLog();
         
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
        XrdOucStream Config(log, theINS, &myEnv, "=====> PurgeQuota ");

        int fd;
        if ((fd = open(config_filename, O_RDONLY, 0)) < 0)
        {
            log->Emsg("Config() can't open configuration file ", config_filename);
        }

        Config.Attach(fd);
        static const char *cvec[] = {"*** pfc purge plugin :", 0};
        Config.Capture(cvec);

        char *var;
        while ((var = Config.GetMyFirstWord()))
        {
            std::string dirpath = var;
            const char* val;

            if (!(val = Config.GetWord()))
            {
                log->Emsg("PurgeQuota plugin", "quota not specified");
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
/*                          XrdPfcGetPurgePin                                 */
/******************************************************************************/

// Return a purge object to use.
extern "C"
{
    XrdPfc::PurgePin *XrdPfcGetPurgePin(XrdSysError &)
    {
        return new XrdPfcPurgeQuota();
    }
}
