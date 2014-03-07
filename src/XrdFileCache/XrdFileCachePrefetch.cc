//----------------------------------------------------------------------------------
// Copyright (c) 2014 by Board of Trustees of the Leland Stanford, Jr., University
// Author: Alja Mrak-Tadel, Matevz Tadel, Brian Bockelman
//----------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//----------------------------------------------------------------------------------

#include <stdio.h>
#include <sstream>
#include <fcntl.h>

#include "XrdCl/XrdClLog.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdOss/XrdOss.hh"
#include "XrdCl/XrdClFile.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdSys/XrdSysTimer.hh"
#include "XrdOuc/XrdOucEnv.hh"


#include "XrdFileCachePrefetch.hh"
#include "XrdFileCacheFactory.hh"
#include "XrdFileCache.hh"
   const static int PREFETCH_MAX_ATTEMPTS = 10;
using namespace XrdFileCache;

bool POK = 0;
bool PFALSE = 0;

Prefetch::RAM::RAM(): m_numBlocks(0),m_buffer(0),  m_blockStates(0)
{
   m_numBlocks = Factory::GetInstance().RefConfiguration().m_NRamBuffers;
   m_buffer = (char*)malloc(m_numBlocks * Factory::GetInstance().RefConfiguration().m_bufferSize);
   m_blockStates = new bool[m_numBlocks];

   for (int i=0; i < m_numBlocks; ++i) m_blockStates[i] = 0;
}

Prefetch::RAM::~RAM()
{
   free(m_buffer);
   delete [] m_blockStates;
}

Prefetch::Prefetch(XrdOucCacheIO &inputIO, std::string& disk_file_path, long long iOffset, long long iFileSize) :
   m_output(NULL),
   m_infoFile(NULL),
   m_input(inputIO),
   m_temp_filename(disk_file_path),
   m_offset(iOffset),
   m_fileSize(iFileSize),
   m_started(false),
   m_failed(false),
   m_stopping(false),
   m_stopped(false),
   m_stateCond(0),    // We will explicitly lock the condition before use.
   m_queueMutex(0)
{
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Prefetch() %s", m_input.Path());
}

//______________________________________________________________________________
Prefetch::~Prefetch()
{
   // see if we have to shut down
   clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch()");

   if (m_started == false) return;

   m_stopping = true;

   while (true)
   {
      m_queueMutex.Lock();
      bool tempty = m_tasks_queue.empty();
      m_queueMutex.UnLock();

      if (m_stopped && tempty)
      {
         clLog()->Debug(XrdCl::AppMsg, "Prefetch::~Prefetch sleep, waiting queues to empty begin");
         bool writewait = false;
         for (int i = 0; i < m_ram.m_numBlocks;++i ) {
            if (m_ram.m_blockStates[i]) {
               writewait = true;
               break;
            }
         }

         clLog()->Debug(XrdCl::AppMsg, "Prefetch::~Prefetch sleep, writew  = %d");
         if (writewait == false) 
            break;
      }
      XrdSysTimer::Wait(100);
   }

   

   // write statistics in *cinfo file
   AppendIOStatToFileInfo();

   clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch close data file");

   if (m_output)
   {
      m_output->Close();
      delete m_output;
      m_output = NULL;
   }
   if (m_infoFile)
   {
      RecordDownloadInfo();
      clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch close info file");

      m_infoFile->Close();
      delete m_infoFile;
      m_infoFile = NULL;
   }
}

//______________________________________________________________________________

bool Prefetch::Open()
{
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Open() open file for disk cache %s", m_input.Path());
   XrdOss  &m_output_fs =  *Factory::GetInstance().GetOss();
   // Create the data file itself.
   XrdOucEnv myEnv;
   m_output_fs.Create(Factory::GetInstance().RefConfiguration().m_username.c_str(), m_temp_filename.c_str(), 0600, myEnv, XRDOSS_mkpath);
   m_output = m_output_fs.newFile(Factory::GetInstance().RefConfiguration().m_username.c_str());
   if (m_output)
   {
      int res = m_output->Open(m_temp_filename.c_str(), O_RDWR, 0600, myEnv);
      if ( res < 0)
      {
         clLog()->Error(XrdCl::AppMsg, "Prefetch::Open() can't get data-FD for %s %s", m_temp_filename.c_str(), m_input.Path());
         delete m_output;
         m_output = NULL;
         return false;
      }
   }
   // Create the info file
   std::string ifn = m_temp_filename + Info::m_infoExtension;
   m_output_fs.Create(Factory::GetInstance().RefConfiguration().m_username.c_str(), ifn.c_str(), 0600, myEnv, XRDOSS_mkpath);
   m_infoFile = m_output_fs.newFile(Factory::GetInstance().RefConfiguration().m_username.c_str());
   if (m_infoFile)
   {

      int res = m_infoFile->Open(ifn.c_str(), O_RDWR, 0600, myEnv);
      if ( res < 0 )
      {
         clLog()->Error(XrdCl::AppMsg, "Prefetch::Open() can't get info-FD %s  %s", ifn.c_str(), m_input.Path());
         delete m_output;
         m_output = NULL;
         delete m_infoFile;
         m_infoFile = NULL;

         return false;
      }
   }
   if ( m_cfi.Read(m_infoFile) <= 0)
   {
      assert(m_fileSize > 0);
      int ss = (m_fileSize -1)/m_cfi.GetBufferSize() + 1;
      clLog()->Info(XrdCl::AppMsg, "Creating new file info with size %lld. Reserve space for %d blocks %s", m_fileSize,  ss, m_input.Path());
      m_cfi.ResizeBits(ss);
      RecordDownloadInfo();
   }
   else
   {
      clLog()->Debug(XrdCl::AppMsg, "Info file already exists %s", m_input.Path());
      // m_cfi.Print();
   }

   return true;
}


//_________________________________________________________________________________________________
void 
Prefetch::Run()
{
   {
      XrdSysCondVarHelper monitor(m_stateCond);
      if (m_started)
      {
         return;
      }

      if ( !Open())
      {
         m_failed = true;
      }
      m_started = true;
      // Broadcast to possible io-read waiting objects
      m_stateCond.Broadcast();

      if (m_failed) return;
   }
   assert(m_infoFile);
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() %s", m_input.Path());

   Task* task;
   int numReadBlocks = 0;
   while ((task = GetNextTask()) != 0)
   { 
      bool already;
      m_downloadStatusMutex.Lock();
      already = m_cfi.TestBit(task->fileBlockIdx);
      m_downloadStatusMutex.UnLock();
      if (already)
      {
         clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() block [%d] already done, continue ... %s",task->fileBlockIdx , m_input.Path());
         
         if (task->condVar)
         {
            clLog()->Warning(XrdCl::AppMsg, "Prefetch::Run() signal already downloaded");

            XrdSysCondVarHelper(*task->condVar);
            task->condVar->Signal();
         }
         continue;
      }
      else
      {
         clLog()->Dump(XrdCl::AppMsg, "Prefetch::Run() download block [%d] %s", task->fileBlockIdx, m_input.Path());
      }

      DoTask(task);
      if (task->condVar)
      {
         XrdSysCondVarHelper(*task->condVar);
         task->condVar->Signal();
      }
      delete task;

      numReadBlocks++;
      if (numReadBlocks % 10)
         RecordDownloadInfo();

      if (m_stopping)
      {
         clLog()->Dump(XrdCl::AppMsg, "Prefetch::Run() stopping for a clean cause");
         break;
      }

   }  // loop tasks

   m_downloadStatusMutex.Lock();
   m_cfi.CheckComplete();
   m_downloadStatusMutex.UnLock();

   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() exits, download %s  !", m_cfi.IsComplete() ? " completed " : "unfinished %s", m_input.Path());

   RecordDownloadInfo();

   m_stopped = true;
} // end Run()


//_____________________________________________________________________________
Prefetch::Task*
Prefetch::GetNextTask()
{
   clLog()->Dump(XrdCl::AppMsg, "Prefetch::GetNextTask() begin ");

   m_queueMutex.Lock();

   while (m_tasks_queue.empty())
   {
      if (m_stopping) return false;

      if (m_queueMutex.WaitMS(500))
      {
         m_queueMutex.UnLock(); 

         m_downloadStatusMutex.Lock();

         Task *task = new Task;
         Task &t = * task;
         t.fileBlockIdx = -1;
         t.ramBlockIdx = -1;
         for (int i = 0; i < m_cfi.GetSizeInBits(); ++i)
         {
            if (m_cfi.TestBit(i) == false)
            {
               t.fileBlockIdx = i;
               t.condVar = 0;
               break;
            }
         }
         m_downloadStatusMutex.UnLock();

         if (t.fileBlockIdx == -1)
         {
            // download complete ?? AMT this should be handled differently
            clLog()->Dump(XrdCl::AppMsg, "Prefetch::GetNextTask() file completed (blockssize  = %d) %s",m_cfi.GetSizeInBits(), m_input.Path());
            delete task;
            return 0;
         }

         if (t.fileBlockIdx >= 0 )
         {
            if (Cache::HaveFreeWritingSlots())
            {
               m_ram.m_writeMutex.Lock();
               for (int i =0 ; i < m_ram.m_numBlocks; ++i) 
               {
                  if (m_ram.m_blockStates[i] == 0)
                  {
                     t.ramBlockIdx = i;
                     break;
                  }
               }

               m_ram.m_writeMutex.UnLock();
            }
         }

         if (t.ramBlockIdx >= 0)
         {
            if (t.fileBlockIdx == (m_cfi.GetSizeInBits() -1))
               t.size =  m_input.FSize() - t.fileBlockIdx*m_cfi.GetBufferSize(); 
            else 
               t.size = m_cfi.GetBufferSize();

            clLog()->Dump(XrdCl::AppMsg, "Prefetch::GetNextTask() read first unread block=%d ramIdx=%d size = %d", t.fileBlockIdx, t.ramBlockIdx, t.size );
            return task;
         }

         clLog()->Dump(XrdCl::AppMsg, "Prefetch::GetNextTask() [%d] no resources, reentering. File %s", t.fileBlockIdx, m_input.Path());
      }
   }

   Task *task = m_tasks_queue.pop_front();
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::GetNextTask() [%d] from queue %s", task->fileBlockIdx, m_input.Path());
   m_queueMutex.UnLock(); 
   return task;   
}

//______________________________________________________________________________
void
Prefetch::DoTask(Task* task)
{ 
   // read block from client  into buffer

   long long offset = task->fileBlockIdx * m_cfi.GetBufferSize();
   int missing =  task->size;
   int cnt = 0;
   char* buff = m_ram.m_buffer;
   buff += task->ramBlockIdx * m_cfi.GetBufferSize();
   while (missing)
   {
      clLog()->Dump(XrdCl::AppMsg, "Prefetch::DoTask() too for block %d singal = %d \n %s", task->ramBlockIdx, task->condVar ? 1:0,  m_input.Path());
      int retval = m_input.Read(buff, offset + m_offset, missing);
      if (retval < 0)
      {
         clLog()->Warning(XrdCl::AppMsg, "Prefetch::DoTask() failed for negative ret %d block %d %s", retval, task->fileBlockIdx , m_input.Path());
         XrdSysCondVarHelper monitor(m_stateCond);
         break;
      }

      missing -= retval;
      offset  += retval;
      buff    += retval;
      ++cnt;
      if (cnt > PREFETCH_MAX_ATTEMPTS)
      {
         break;
      }
   }

    if (missing == 0)
   {
      // queue for ram to disk write
      Cache::AddWriteTask(this, task->ramBlockIdx, task->fileBlockIdx, task->size);
      if (task->ok)
         *task->ok = true;
   }
   else
   {
      clLog()->Dump(XrdCl::AppMsg, "Prefetch::DoTask() incomplete read missing %d for block %d %s", missing, task->fileBlockIdx, m_input.Path());
   }
}

//_________________________________________________________________________________________________
void 
Prefetch::WriteBlockToDisk(int ramIdx, int fileIdx, size_t size)
{
   // called from XrdFileCache::Cache when process queue

   // AMT howto make this locking ?
   char* buff = m_ram.m_buffer;
   buff += ramIdx*m_cfi.GetBufferSize();
   int retval = 0;


   // write block buffer into disk file
   long long offset = fileIdx * m_cfi.GetBufferSize();
   int buffer_remaining = size;
   int buffer_offset = 0;
   int cnt = 0;
   while ((buffer_remaining > 0) && // There is more to be written
          (((retval = m_output->Write(buff, offset + buffer_offset, buffer_remaining)) != -1)
           || (errno == EINTR))) // Write occurs without an error
   {
      buffer_remaining -= retval;
      buff += retval;
      cnt++;

      if (buffer_remaining)        
            clLog()->Warning(XrdCl::AppMsg, "Prefetch::WriteToDisk() reattempt[%d] writing missing %d for block %d %s", cnt,  buffer_remaining, fileIdx, m_input.Path());

      if (cnt > PREFETCH_MAX_ATTEMPTS )
      {
            clLog()->Error(XrdCl::AppMsg, "Prefetch::WriteToDisk() write failes too manny attempts %s",  m_input.Path());
      }

   }


   // set downloaded bits
   clLog()->Dump(XrdCl::AppMsg, "Prefetch::WriteToDisk() success set bit for block [%d] size [%d] %s", fileIdx, size, m_input.Path());
   m_downloadStatusMutex.Lock();
   m_cfi.SetBit(fileIdx);
   m_downloadStatusMutex.UnLock();

   // mark ram block available
   m_ram.m_writeMutex.Lock();
   m_ram.m_blockStates[ramIdx] = 0;
   m_ram.m_writeMutex.UnLock();
}

//______________________________________________________________________________
bool Prefetch::ReadFromTask(int iFileBlockIdx, char* iBuff, long long iOff, size_t iSize)
{
   // offs == offset inside the block, size  read size in block
   clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadFromTask fileIdx= %d begin", iFileBlockIdx); 
   if (Cache::HaveFreeWritingSlots())
   {
      int ramIdx = -1;
      m_ram.m_writeMutex.Lock();
      for (int i =0 ; i < m_ram.m_numBlocks; ++i)
      {
         if (m_ram.m_blockStates[i] == 0)
         {
            ramIdx = i;
            break;
         }
      }
      m_ram.m_writeMutex.UnLock();

      if (ramIdx >= 0) 
      {
         // create task, check if this is the end block
         size_t taskSize = m_cfi.GetBufferSize();
         if (iFileBlockIdx == (m_cfi.GetSizeInBits() -1))
         {
            taskSize = m_input.FSize() - iFileBlockIdx*m_cfi.GetBufferSize();
         }

         clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadFromTask, going to add task fileIdx=%d ", iFileBlockIdx); 
         XrdSysCondVar newTaskCond(0);
         bool clientreadOK = false;
         {
            XrdSysCondVarHelper xx(newTaskCond);

            m_queueMutex.Lock();
            m_tasks_queue.push_front(new Task(iFileBlockIdx, ramIdx, taskSize, &newTaskCond, &clientreadOK));

            m_queueMutex.Signal();
            m_queueMutex.UnLock();

            newTaskCond.Wait();
         }
         clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadFromTask memcpy from RAM to IO::buffer fileIdx=%d ", iFileBlockIdx);
         if (clientreadOK)
         {
            long long inBlockOff = iOff - iFileBlockIdx * m_cfi.GetBufferSize();
            char* srcBuff =  m_ram.m_buffer  + ramIdx*m_cfi.GetBufferSize(); 
            memcpy(iBuff, srcBuff + inBlockOff, iSize);
            return true;
         }
         else
         {
            clLog()->Error(XrdCl::AppMsg, "Prefetch::ReadFromTask client fileIdx=%d failed", iFileBlockIdx); 
            return false;
         }
      }
      clLog()->Debug(XrdCl::AppMsg, "Prefetch::ReadFromTask can't get free ram, not enough resources");
   }
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::ReadFromTask write queue full, not enough resources");
   return false;
}


//______________________________________________________________________________

ssize_t Prefetch::ReadInBlocks(char *buff, off_t off, size_t size)
{

   long long off0 = off;
   int idx_first = off0 / m_cfi.GetBufferSize();
   int idx_last  = (off0 + size -1)/ m_cfi.GetBufferSize();

   size_t bytes_read = 0;
   for (int blockIdx = idx_first; blockIdx <= idx_last; ++blockIdx )
   {
      int readBlockSize = size;
      if (idx_first != idx_last)
      {
         if (blockIdx == idx_first)
         {
            readBlockSize = (blockIdx + 1) * m_cfi.GetBufferSize() - off0;
            clLog()->Dump(XrdCl::AppMsg, "Read partially till the end of the block %", m_input.Path());
         }
         else if (blockIdx == idx_last)
         {
            readBlockSize = (off0+size) - blockIdx*m_cfi.GetBufferSize();
            clLog()->Dump(XrdCl::AppMsg, "Read partially from beginning of block %s", m_input.Path());
         }
         else
         {
            readBlockSize = m_cfi.GetBufferSize();
         }
      }

      if (readBlockSize > m_cfi.GetBufferSize()) {
         clLog()->Error(XrdCl::AppMsg, "block size invalid");
      }

      int retvalBlock = -1;
      // now do per block read at Read(buff, off, readBlockSize)

      m_downloadStatusMutex.Lock();
      bool dsl = m_cfi.TestBit(blockIdx);
      m_downloadStatusMutex.UnLock(); 

      if (dsl)
      {
         retvalBlock = m_output->Read(buff, off, readBlockSize);
         m_stats.m_BytesDisk += retvalBlock;
         clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadInBlocks [%d] disk = %d",blockIdx, retvalBlock);
      }
      else 
      {
         if (ReadFromTask(blockIdx, buff, off, readBlockSize))
         {
            retvalBlock = readBlockSize; // presume since ReadFromTask did not fail, could pass a refrence to ReadFromTask
            m_stats.m_BytesRam += retvalBlock;
            clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadInBlocks [%d]  ram = %d", blockIdx, retvalBlock);
         }
         else
         {
            retvalBlock = m_input.Read(buff, off, readBlockSize);
            clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadInBlocks [%d]  client = %d", blockIdx, retvalBlock);
            m_stats.m_BytesMissed += retvalBlock;
         }
      }

      if (retvalBlock > 0 )
      {
         bytes_read += retvalBlock;
         buff       += retvalBlock;
         off        += retvalBlock;
         if (readBlockSize != retvalBlock)
         {
            clLog()->Warning(XrdCl::AppMsg, "Prefetch::ReadInBlocks incomplete , missing = %d", readBlockSize-retvalBlock);
            return bytes_read;
         }
      }
      else
      {
         return bytes_read;
      }
   }
   return bytes_read;
}

//______________________________________________________________________________
ssize_t 
Prefetch::Read(char *buff, off_t off, size_t size)
{ 
   {
      XrdSysCondVarHelper monitor(m_stateCond);

      // AMT check if this can be done once during initalization
      if (m_failed) return 0;
      
      if ( ! m_started)
      {
         m_stateCond.Wait();
         if (m_failed) return 0;
      }
   }

   clLog()->Dump(XrdCl::AppMsg, "Prefetch::Read()  off = %lld size = %lld. %s", off, size, m_input.Path());

   bool fileComplete;
   m_downloadStatusMutex.Lock();
   fileComplete = m_cfi.IsComplete();
   m_downloadStatusMutex.UnLock();

   if (fileComplete)
   {  
      int res = m_output->Read(buff, off, size);
      m_stats.m_BytesDisk += res;
      return res;
   }
   else
   {
      if (m_stopping)
         return 0;
      else
         return ReadInBlocks(buff, off, size);
   }
}

//______________________________________________________________________________
void Prefetch::RecordDownloadInfo()
{
   clLog()->Debug(XrdCl::AppMsg, "Prefetch record Info file %s", m_input.Path());
   m_cfi.WriteHeader(m_infoFile);
   m_infoFile->Fsync();
}

//______________________________________________________________________________
void Prefetch::AppendIOStatToFileInfo()
{
   // lock in case several IOs want to write in *cinfo file
   m_downloadStatusMutex.Lock();
   if (m_infoFile)
   {
      m_cfi.AppendIOStat(&m_stats, (XrdOssDF*)m_infoFile);
   }
   else
   {
      clLog()->Warning(XrdCl::AppMsg, "Prefetch::AppendIOStatToFileInfo() info file not opened %s", m_input.Path());
   }
   m_downloadStatusMutex.UnLock();
}
