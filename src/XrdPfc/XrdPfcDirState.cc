#include "XrdPfcDirState.hh"
#include "XrdPfcPathParseTools.hh"

#include <string>

namespace XrdPfc
{

//----------------------------------------------------------------------------
//! Constructor
//----------------------------------------------------------------------------
DirState::DirState() : m_parent(0), m_depth(0)
{}

//----------------------------------------------------------------------------
//! Constructor
//! @param DirState parent directory
//----------------------------------------------------------------------------
DirState::DirState(DirState *parent) :
    m_parent(parent),
    m_depth(m_parent->m_depth + 1)
{}

//----------------------------------------------------------------------------
//! Constructor
//! @param parent parent DirState object
//! @param dname  name of this directory only, no slashes, no extras.
//----------------------------------------------------------------------------
DirState::DirState(DirState *parent, const std::string& dname) :
    m_parent(parent),
    m_dir_name(dname),
    m_depth(m_parent->m_depth + 1)
{}

//----------------------------------------------------------------------------
//! Internal function called from find_dir or find_path_tok
//! @param dir subdir name
//----------------------------------------------------------------------------
DirState* DirState::create_child(const std::string &dir)
{
    std::pair<DsMap_i, bool> ir = m_subdirs.insert(std::make_pair(dir, DirState(this, dir)));
    return & ir.first->second;
}

//----------------------------------------------------------------------------
//! Internal function called from find_path
//! @param dir subdir name
//----------------------------------------------------------------------------
DirState* DirState::find_path_tok(PathTokenizer &pt, int pos, bool create_subdirs)
{
    if (pos == pt.get_n_dirs()) return this;

    DsMap_i i = m_subdirs.find(pt.m_dirs[pos]);

    DirState *ds = 0;

    if (i != m_subdirs.end())
    {
        ds = & i->second;
    }
    if (create_subdirs)
    {
        ds = create_child(pt.m_dirs[pos]);
    }
    if (ds) return ds->find_path_tok(pt, pos + 1, create_subdirs);

    return 0;
}

//----------------------------------------------------------------------------
//! Recursive function to find DirState with given absolute dir path
//! @param path full path to parse
//! @param max_depth directory depth to which to descend (value < 0 means full descent)
//! @param parse_as_lfn
//! @param create_subdirs 
DirState* DirState::find_path(const std::string &path, int max_depth, bool parse_as_lfn,
                              bool create_subdirs)
{
    PathTokenizer pt(path, max_depth, parse_as_lfn);

    return find_path_tok(pt, 0, create_subdirs);
}

//----------------------------------------------------------------------------
//! Non recursive function to find an entry in this directory only.
//! @param dir subdir name @param bool create the subdir in this DirsStat
//! @param create_subdirs if true and the dir is not found, a new DirState
//!        child is created
DirState* DirState::find_dir(const std::string &dir,
                             bool create_subdirs)
{
    DsMap_i i = m_subdirs.find(dir);

    if (i != m_subdirs.end())  return & i->second;

    if (create_subdirs)  return create_child(dir);

    return 0;
}

//----------------------------------------------------------------------------
//! Reset current transaction statistics.
//! Called from Cache::copy_out_active_stats_and_update_data_fs_state()
//----------------------------------------------------------------------------
void DirState::reset_stats()
{
    m_here_stats.Reset();
    m_recursive_subdirs_stats.Reset();

    for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
    {
        i->second.reset_stats();
    }
}

//----------------------------------------------------------------------------
//! Propagate stat to parents
//! Called from Cache::copy_out_active_stats_and_update_data_fs_state()
//----------------------------------------------------------------------------
void DirState::upward_propagate_stats()
{
    for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
    {
        i->second.upward_propagate_stats();

        // XXXXX m_stats.AddUp(i->second.m_stats);
        // fix these for here_stats, there_stats
    }
}

//----------------------------------------------------------------------------
//! Update statistics.
//! Called from Purge thread.
//----------------------------------------------------------------------------
long long DirState::upward_propagate_usage_purged()
{
    // XXXXX what's with this?
/*
    for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
    {
        m_usage_purged += i->second.upward_propagate_usage_purged();
    }
    m_usage -= m_usage_purged;

    long long ret = m_usage_purged;
    m_usage_purged = 0;
    return ret;
*/
    return 0;
}

//----------------------------------------------------------------------------
//! Recursive print of statistics. Called if defined in pfc configuration.
//!
//----------------------------------------------------------------------------
void DirState::dump_recursively(const char *name, int max_depth)
{
    printf("%*d %s usage=%lld usage_extra=%lld usage_total=%lld num_ios=%d duration=%d b_hit=%lld b_miss=%lld b_byps=%lld b_wrtn=%lld\n",
            2 + 2*m_depth, m_depth, name,
            // XXXXX decide what goes here
            // m_usage, m_usage_extra, m_usage + m_usage_extra,
            0ll, 0ll, 0ll,
            // XXXXX here_stats or sum up?
            m_here_stats.m_NumIos, m_here_stats.m_Duration,
            m_here_stats.m_BytesHit, m_here_stats.m_BytesMissed, m_here_stats.m_BytesBypassed,
            m_here_stats.m_BytesWritten);

    if (m_depth >= max_depth)
        return;

    for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
    {
        i->second.dump_recursively(i->first.c_str(), max_depth);
    }
}

//----------------------------------------------------------------------------
//! Add to temporary Stat obj
//!
//----------------------------------------------------------------------------
void DirState::add_up_stats(const Stats& stats)
{
   m_here_stats.AddUp(stats);
   // XXXX propagate to parent done at the end.
}

} // end namespace
