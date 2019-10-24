#include "XrdSfs/XrdSfsGPFile.hh"
// The native SFS page size
//
#define XrdSfsPageSize 4096

virtual int         autoStat(struct stat *buf);
//-----------------------------------------------------------------------------
//! Create, delete, query, or rollback a file checkpoint.
//!
//! @param  act   - The operation to be performed (see cpAct enum below).
//! @param  range - Portions of the file to be checkpointed.
//! @param  n     - Number of elements in range.
//!
//! @return One of SFS_OK or SFS_ERROR.
//-----------------------------------------------------------------------------

enum cpAct {cpCreate=0,   //!< Create a checkpoint, one must not be active.
            cpDelete,     //!< Delete an existing checkpoint
            cpRestore     //!< Restore an active checkpoint and delete it.
           };

virtual int            checkpoint(cpAct act, struct iov *range=0, int n=0);

                            const XrdSecEntity     *client = 0);
//-----------------------------------------------------------------------------
//! Read file pages into a buffer and return corresponding checksums.
//!
//! @param  offset  - The offset where the read is to start. It must be
//!                   page aligned.
//! @param  buffer  - pointer to buffer where the bytes are to be placed.
//! @param  rdlen   - The number of bytes to read. The amount must be an
//!                   integral number of XrdSfsPageSize bytes.
//! @param  csvec   - A vector of [rdlen/XrdSfsPageSize] entries which will be
//!                   filled with the corresponding CRC32 checksum for each
//!                   page. A nil pointer does not return the checksums.
//! @param  verify  - When true, the checksum is verified for each page; an
//!                   error is returned if any checksum is incorrect.
//!
//! @return >= 0      The number of bytes that placed in buffer.
//! @return SFS_ERROR File could not be read, error holds the reason.
//-----------------------------------------------------------------------------

virtual XrdSfsXferSize pgRead(XrdSfsFileOffset   offset,
                              char              *buffer,
                              XrdSfsXferSize     rdlen,
                              uint32_t          *csvec,
                              bool               verify=true);

//-----------------------------------------------------------------------------
//! Read file pages and checksums using asynchronous I/O.
//!
//! @param  aioparm - Pointer to async I/O object controlling the I/O.
//! @param  verify  - When true, the checksum is verified for each page; an
//!                   error is returned if any checksum is incorrect.
//!
//! @return SFS_OK    Request accepted and will be scheduled.
//! @return SFS_ERROR File could not be read, error holds the reason.
//-----------------------------------------------------------------------------

virtual int            pgRead(XrdSfsAio *aioparm, bool verify=true);

//-----------------------------------------------------------------------------
//! Write file pages into a file with corresponding checksums.
//!
//! @param  offset  - The offset where the write is to start. It must be
//!                   page aligned.
//! @param  buffer  - pointer to buffer containing the bytes to write.
//! @param  wrlen   - The number of bytes to write. If amount is not an
//!                   integral number of XrdSfsPageSize bytes, then this must
//!                   be the last write to the file at or above the offset.
//! @param  csvec   - A vector of [CEILING(wrlen/XrdSfsPageSize)] entries which
//!                   contain the corresponding CRC32 checksum for each page.
//!                   A nil pointer causes the checksums to be computed.
//! @param  verify  - When true, the checksum in csvec is verified for each
//!                   page; and error is returned if any checksum is incorrect.
//!
//! @return >= 0      The number of bytes written.
//! @return SFS_ERROR File could not be read, error holds the reason.
//-----------------------------------------------------------------------------

virtual XrdSfsXferSize pgWrite(XrdSfsFileOffset   offset,
                               char              *buffer,
                               XrdSfsXferSize     wrlen,
                               uint32_t          *csvec,
                               bool               verify=true);

//-----------------------------------------------------------------------------
//! Write file pages and checksums using asynchronous I/O.
//!
//! @param  aioparm - Pointer to async I/O object controlling the I/O.
//! @param  verify  - When true, the checksum in csvec is verified for each
//!                   page; and error is returned if any checksum is incorrect.
//!
//! @return SFS_OK    Request accepted and will be scheduled.
//! @return SFS_ERROR File could not be read, error holds the reason.
//-----------------------------------------------------------------------------

virtual int            pgWrite(XrdSfsAio *aioparm, bool verify=true);

//! Read file bytes using asynchronous I/O.
virtual int            read(XrdSfsAio *aioparm) = 0;
                             int               rdvCnt);
                                XrdSfsXferSize     size);
//! Write file bytes using asynchronous I/O.
                              int               wdvCnt);
                                 {lclEI = &error; pgwrEOF = 0;}
                                 : error(wrapF.error), lclEI(0), pgwrEOF(0) {}
                                 : error(eInfo), lclEI(0), pgwrEOF(0) {}
XrdOucErrInfo*   lclEI;
XrdSfsFileOffset pgwrEOF;
virtual XrdSfsDirectory *newDir(XrdOucErrInfo &eInfo) {(void)eInfo; return 0;}
virtual XrdSfsFile      *newFile(XrdOucErrInfo &eInfo) {(void)eInfo; return 0;}
                              const char             *opaque = 0);
//! Notify filesystem that a client has connected.
virtual void           Connect(const XrdSecEntity     *client = 0)
//-----------------------------------------------------------------------------
//! Notify filesystem that a client has disconnected.
//!
//! @param  client - Client's identify (see common description).
//-----------------------------------------------------------------------------

virtual void           Disc(const XrdSecEntity *client = 0) {(void)client;}

virtual void           EnvInfo(XrdOucEnv *envP) {(void)envP;}

//-----------------------------------------------------------------------------
//! Return directory/file existence information (short stat).
//!
//! @param  path   - Pointer to the path of the file/directory in question.
//! @param  eFlag  - Where the results are to be returned.
//! @param  eInfo  - The object where error info is to be returned.
//! @param  client - Client's identify (see common description).
//! @param  opaque - Path's CGI information (see common description).
//!
//! @return One of SFS_OK, SFS_ERROR, SFS_REDIRECT, SFS_STALL, or SFS_STARTED
//!         When SFS_OK is returned, eFlag must be properly set, as follows:
//!         XrdSfsFileExistNo            - path does not exist
//!         XrdSfsFileExistIsFile        - path refers to an  online file
//!         XrdSfsFileExistIsDirectory   - path refers to an  online directory
//!         XrdSfsFileExistIsOffline     - path refers to an offline file
//!         XrdSfsFileExistIsOther       - path is neither a file nor directory
//-----------------------------------------------------------------------------

virtual int            exists(const char                *path,
                                    XrdSfsFileExistence &eFlag,
                                    XrdOucErrInfo       &eInfo,
                              const XrdSecEntity        *client = 0,
                              const char                *opaque = 0) = 0;
                             const XrdSecEntity     *client = 0);
//! @return The bit-wise feature set (i.e. supported or configured).
//!         See include file XrdSfsFlags.hh for actual bit values.
        uint64_t       Features() {return FeatureSet;}
                             const XrdSecEntity     *client = 0);
//! Return maximum checkpoint size.
//! @return Maximum size of a checkpoint.
virtual int            getChkPSize() {return 0;}
//! Perform a third party file transfer or cancel one.
//! @param  gpAct  - What to do as one of the enums listed below.
//! @param  gpReq  - reference tothe object describing the request. This object
//!                  is also used communicate the request status.
//! @param  eInfo  - The object where error info or results are to be returned.
//! @return SFS_OK   Request accepted (same as SFS_STARTED). Otherwise, one of
//!                  SFS_ERROR, SFS_REDIRECT, or SFS_STALL.
enum gpfFunc {gpfCancel=0, //!< Cancel this request
              gpfGet,      //!< Perform a file retrieval
              gpfPut       //!< Perform a file push
             };

virtual int            gpFile(      gpfFunc          &gpAct,
                                    XrdSfsGPFile     &gpReq,
                                    XrdOucErrInfo    &eInfo,
                              const XrdSecEntity     *client = 0);
//! Prepare a file for future processing.
                       XrdSfsFileSystem();

protected:

uint64_t               FeatureSet; //!< Adjust features at initialization
    @param  envP     - Pointer to the environment containing implementation
                       specific information.
/*! The old-style entry-point is still supported as a fallback. Should the
    version '2' entry point is not found, the system attempts to use the
    version '1' entry point.

    extern "C"
         {XrdSfsFileSystem *XrdSfsGetFileSystem(XrdSfsFileSystem *nativeFS,
                                                XrdSysLogger     *Logger,
                                                const char       *configFn);
         }
*/

typedef XrdSfsFileSystem *(*XrdSfsFileSystem_t) (XrdSfsFileSystem *nativeFS,
                                                 XrdSysLogger     *Logger,
                                                 const char       *configFn);
