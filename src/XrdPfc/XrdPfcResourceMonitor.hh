#ifndef __XRDPFC_RESOURCEMONITOR_HH__
#define __XRDPFC_RESOURCEMONITOR_HH__

#include "XrdPfcStats.hh"

#include "XrdSys/XrdSysPthread.hh"

#include <string>
#include <vector>

class XrdOss;

namespace XrdPfc {

class DataFsState;
class DirState;
class FsTraversal;

//==============================================================================
// ResourceMonitor
//==============================================================================

// Encapsulates local variables used withing the previous mega-function Purge().
//
// This will be used within the continuously/periodically ran heart-beat / breath
// function ... and then parts of it will be passed to invoked FS scan and purge
// jobs (which will be controlled throught this as well).

// Andy: XRDADMINPATH Is the directory for administrative files (i.e. all.adminpath)
// Also: XrdOucEnv::Export("XRDLOGDIR", logParms.logfn); (in XrdOucLogging::configLog)

class ResourceMonitor
{
   template<typename ID, typename RECORD>
   class Queue {
   public:
      struct Entry {
         ID      id;
         RECORD  record;
      };
      using queue_type = std::vector<Entry>;
      using iterator   = typename queue_type::iterator;

      Queue() = default;

      int  write_queue_size() const { return m_write_queue.size(); }
      bool read_queue_empty() const { return m_read_queue.empty(); }
      int  read_queue_size()  const { return m_read_queue.size(); }

      // Writer / producer access
      void push(ID id, RECORD stat) { m_write_queue.push_back({ id, stat }); }

      // Reader / consumer access
      int swap_queues() { m_read_queue.clear(); m_write_queue.swap(m_read_queue); return read_queue_size(); }
      const queue_type& read_queue() const { return m_read_queue; }
      iterator begin() const { return m_read_queue.begin(); }
      iterator end()   const { return m_read_queue.end(); }

   private:
      queue_type m_write_queue, m_read_queue;
   };

   struct AccessToken {
      std::string  m_filename;
      DirState    *m_dir_state = nullptr;

      void clear() { m_filename.clear(); m_dir_state = nullptr; }
   };
   std::vector<AccessToken> m_access_tokens;
   std::vector<int>         m_access_tokens_free_slots;

   struct OpenRecord {
      // new file? complete?
      time_t m_open_time;
   };

   struct CloseRecord {
      Stats  m_stats;
      time_t m_close_time;
   };

   struct PurgeRecord {
      long long m_total_size;
      int n_files;
   };

   Queue<int, OpenRecord>        m_file_open_q; // filename is also in the token
   Queue<int, CloseRecord>       m_file_close_q;
   Queue<DirState*,   PurgeRecord> m_file_purge_q1;
   Queue<std::string, PurgeRecord> m_file_purge_q2;
   Queue<std::string, long long>   m_file_purge_q3;
   // DirPurge queue -- not needed? But we do need last-change timestamp in DirState.

   // Updates from active files are pulled in from Cache.
   std::vector<std::pair<int, Stats>> m_file_stats_update_vec;

   XrdSysMutex m_queue_mutex; // mutex shared between queues
 
   DataFsState *m_fs_state;
   XrdOss      &m_oss;

public:
   ResourceMonitor(XrdOss& oss);
   ~ResourceMonitor();

   // --- Initial scan, building of DirState tree

   void scan_dir_and_recurse(FsTraversal &fst);
   bool perform_initial_scan();

   // --- Event registration

   int register_file_open(const std::string& filename, time_t open_timestamp) {
      // Simply return a token, we will resolve it in the actual processing of the queue.
      XrdSysMutexHelper _lock(&m_queue_mutex);
      int token;
      if ( ! m_access_tokens_free_slots.empty()) {
         token = m_access_tokens_free_slots.back();
         m_access_tokens_free_slots.pop_back();
         m_access_tokens[token].m_filename = filename;
      } else {
         token = (int) m_access_tokens.size();
         m_access_tokens.push_back({filename});
      }

      m_file_open_q.push(token, {open_timestamp});
      return token;
   }

   void register_file_close(int token, Stats stats, time_t close_timestamp) {
      XrdSysMutexHelper _lock(&m_queue_mutex);
      m_file_close_q.push(token, {stats, close_timestamp});
   }

   // deletions can come from purge and from direct requests (Cache::UnlinkFile), the latter
   // also covering the emergency shutdown of a file.
   void register_file_purge(DirState* target, long long file_size) {
      XrdSysMutexHelper _lock(&m_queue_mutex);
      m_file_purge_q1.push(target, {file_size, 1});
   }
   void register_multi_file_purge(DirState* target, long long file_size, int n_files) {
      XrdSysMutexHelper _lock(&m_queue_mutex);
      m_file_purge_q1.push(target, {file_size, n_files});
   }
   void register_multi_file_purge(const std::string& target, long long file_size, int n_files) {
      XrdSysMutexHelper _lock(&m_queue_mutex);
      m_file_purge_q2.push(target, {file_size, n_files});
   }
   void register_file_purge(const std::string& filename, long long file_size) {
      XrdSysMutexHelper _lock(&m_queue_mutex);
      m_file_purge_q3.push(filename, file_size);
   }

   // void register_dir_purge(DirState* target);
   // target assumed to be empty at this point, triggered by a file_purge removing the last file in it.
   // hmmh, this is actually tricky ... who will purge the dirs? we should now at export-to-vector time
   // and can prune leaf directories. Tthis might fail if a file has been created in there in the meantime, which is ok.
   // However, is there a race condition between rmdir and creation of a new file in that dir? Ask Andy.

   // --- Helpers for event processing and actions

   AccessToken& token(int i) { return m_access_tokens[i]; }

   // --- Actions

   int  process_queues();

   void heart_beat();

   /* XXX Stuff from Cache, to be revisited.
   enum ScanAndPurgeThreadState_e { SPTS_Idle, SPTS_Scan, SPTS_Purge, SPTS_Done };

   XrdSysCondVar    m_stats_n_purge_cond; //!< communication between heart-beat and scan-purge threads

   int                       m_last_scan_duration;
   int                       m_last_purge_duration;
   ScanAndPurgeThreadState_e m_spt_state;
   // ---
   m_stats_n_purge_cond(0),
   m_last_scan_duration(0),
   m_last_purge_duration(0),
   m_spt_state(SPTS_Idle)

   */
};

}

#endif
