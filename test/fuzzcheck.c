/*
** 2015-05-25
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This is a utility program designed to aid running regressions tests on
** the SQLite library using data from an external fuzzer, such as American
** Fuzzy Lop (AFL) (http://lcamtuf.coredump.cx/afl/).
**
** This program reads content from an SQLite database file with the following
** schema:
**
**     CREATE TABLE db(
**       dbid INTEGER PRIMARY KEY, -- database id
**       dbcontent BLOB            -- database disk file image
**     );
**     CREATE TABLE xsql(
**       sqlid INTEGER PRIMARY KEY,   -- SQL script id
**       sqltext TEXT                 -- Text of SQL statements to run
**     );
**     CREATE TABLE IF NOT EXISTS readme(
**       msg TEXT -- Human-readable description of this test collection
**     );
**
** For each database file in the DB table, the SQL text in the XSQL table
** is run against that database.  All README.MSG values are printed prior
** to the start of the test (unless the --quiet option is used).  If the
** DB table is empty, then all entries in XSQL are run against an empty
** in-memory database.
**
** This program is looking for crashes, assertion faults, and/or memory leaks.
** No attempt is made to verify the output.  The assumption is that either all
** of the database files or all of the SQL statements are malformed inputs,
** generated by a fuzzer, that need to be checked to make sure they do not
** present a security risk.
**
** This program also includes some command-line options to help with 
** creation and maintenance of the source content database.  The command
**
**     ./fuzzcheck database.db --load-sql FILE...
**
** Loads all FILE... arguments into the XSQL table.  The --load-db option
** works the same but loads the files into the DB table.  The -m option can
** be used to initialize the README table.  The "database.db" file is created
** if it does not previously exist.  Example:
**
**     ./fuzzcheck new.db --load-sql *.sql
**     ./fuzzcheck new.db --load-db *.db
**     ./fuzzcheck new.db -m 'New test cases'
**
** The three commands above will create the "new.db" file and initialize all
** tables.  Then do "./fuzzcheck new.db" to run the tests.
**
** DEBUGGING HINTS:
**
** If fuzzcheck does crash, it can be run in the debugger and the content
** of the global variable g.zTextName[] will identify the specific XSQL and
** DB values that were running when the crash occurred.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "sqlite3.h"

#ifdef __unix__
# include <signal.h>
# include <unistd.h>
#endif

/*
** Files in the virtual file system.
*/
typedef struct VFile VFile;
struct VFile {
  char *zFilename;        /* Filename.  NULL for delete-on-close. From malloc() */
  int sz;                 /* Size of the file in bytes */
  int nRef;               /* Number of references to this file */
  unsigned char *a;       /* Content of the file.  From malloc() */
};
typedef struct VHandle VHandle;
struct VHandle {
  sqlite3_file base;      /* Base class.  Must be first */
  VFile *pVFile;          /* The underlying file */
};

/*
** The value of a database file template, or of an SQL script
*/
typedef struct Blob Blob;
struct Blob {
  Blob *pNext;            /* Next in a list */
  int id;                 /* Id of this Blob */
  int seq;                /* Sequence number */
  int sz;                 /* Size of this Blob in bytes */
  unsigned char a[1];     /* Blob content.  Extra space allocated as needed. */
};

/*
** Maximum number of files in the in-memory virtual filesystem.
*/
#define MX_FILE  10

/*
** Maximum allowed file size
*/
#define MX_FILE_SZ 10000000

/*
** All global variables are gathered into the "g" singleton.
*/
static struct GlobalVars {
  const char *zArgv0;              /* Name of program */
  VFile aFile[MX_FILE];            /* The virtual filesystem */
  int nDb;                         /* Number of template databases */
  Blob *pFirstDb;                  /* Content of first template database */
  int nSql;                        /* Number of SQL scripts */
  Blob *pFirstSql;                 /* First SQL script */
  char zTestName[100];             /* Name of current test */
} g;

/*
** Print an error message and quit.
*/
static void fatalError(const char *zFormat, ...){
  va_list ap;
  if( g.zTestName[0] ){
    fprintf(stderr, "%s (%s): ", g.zArgv0, g.zTestName);
  }else{
    fprintf(stderr, "%s: ", g.zArgv0);
  }
  va_start(ap, zFormat);
  vfprintf(stderr, zFormat, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

/*
** Timeout handler
*/
#ifdef __unix__
static void timeoutHandler(int NotUsed){
  (void)NotUsed;
  fatalError("timeout\n");
}
#endif

/*
** Set the an alarm to go off after N seconds.  Disable the alarm
** if N==0
*/
static void setAlarm(int N){
#ifdef __unix__
  alarm(N);
#else
  (void)N;
#endif
}

/*
** This an SQL progress handler.  After an SQL statement has run for
** many steps, we want to interrupt it.  This guards against infinite
** loops from recursive common table expressions.
**
** *pVdbeLimitFlag is true if the --limit-vdbe command-line option is used.
** In that case, hitting the progress handler is a fatal error.
*/
static int progressHandler(void *pVdbeLimitFlag){
  if( *(int*)pVdbeLimitFlag ) fatalError("too many VDBE cycles");
  return 1;
}

/*
** Reallocate memory.  Show and error and quit if unable.
*/
static void *safe_realloc(void *pOld, int szNew){
  void *pNew = realloc(pOld, szNew);
  if( pNew==0 ) fatalError("unable to realloc for %d bytes", szNew);
  return pNew;
}

/*
** Initialize the virtual file system.
*/
static void formatVfs(void){
  int i;
  for(i=0; i<MX_FILE; i++){
    g.aFile[i].sz = -1;
    g.aFile[i].zFilename = 0;
    g.aFile[i].a = 0;
    g.aFile[i].nRef = 0;
  }
}


/*
** Erase all information in the virtual file system.
*/
static void reformatVfs(void){
  int i;
  for(i=0; i<MX_FILE; i++){
    if( g.aFile[i].sz<0 ) continue;
    if( g.aFile[i].zFilename ){
      free(g.aFile[i].zFilename);
      g.aFile[i].zFilename = 0;
    }
    if( g.aFile[i].nRef>0 ){
      fatalError("file %d still open.  nRef=%d", i, g.aFile[i].nRef);
    }
    g.aFile[i].sz = -1;
    free(g.aFile[i].a);
    g.aFile[i].a = 0;
    g.aFile[i].nRef = 0;
  }
}

/*
** Find a VFile by name
*/
static VFile *findVFile(const char *zName){
  int i;
  if( zName==0 ) return 0;
  for(i=0; i<MX_FILE; i++){
    if( g.aFile[i].zFilename==0 ) continue;   
    if( strcmp(g.aFile[i].zFilename, zName)==0 ) return &g.aFile[i];
  }
  return 0;
}

/*
** Find a VFile by name.  Create it if it does not already exist and
** initialize it to the size and content given.
**
** Return NULL only if the filesystem is full.
*/
static VFile *createVFile(const char *zName, int sz, unsigned char *pData){
  VFile *pNew = findVFile(zName);
  int i;
  if( pNew ) return pNew;
  for(i=0; i<MX_FILE && g.aFile[i].sz>=0; i++){}
  if( i>=MX_FILE ) return 0;
  pNew = &g.aFile[i];
  if( zName ){
    pNew->zFilename = safe_realloc(0, strlen(zName)+1);
    memcpy(pNew->zFilename, zName, strlen(zName)+1);
  }else{
    pNew->zFilename = 0;
  }
  pNew->nRef = 0;
  pNew->sz = sz;
  pNew->a = safe_realloc(0, sz);
  if( sz>0 ) memcpy(pNew->a, pData, sz);
  return pNew;
}


/*
** Implementation of the "readfile(X)" SQL function.  The entire content
** of the file named X is read and returned as a BLOB.  NULL is returned
** if the file does not exist or is unreadable.
*/
static void readfileFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  const char *zName;
  FILE *in;
  long nIn;
  void *pBuf;

  zName = (const char*)sqlite3_value_text(argv[0]);
  if( zName==0 ) return;
  in = fopen(zName, "rb");
  if( in==0 ) return;
  fseek(in, 0, SEEK_END);
  nIn = ftell(in);
  rewind(in);
  pBuf = sqlite3_malloc64( nIn );
  if( pBuf && 1==fread(pBuf, nIn, 1, in) ){
    sqlite3_result_blob(context, pBuf, nIn, sqlite3_free);
  }else{
    sqlite3_free(pBuf);
  }
  fclose(in);
}

/*
** Load a list of Blob objects from the database
*/
static void blobListLoadFromDb(
  sqlite3 *db,             /* Read from this database */
  const char *zSql,        /* Query used to extract the blobs */
  int onlyId,              /* Only load where id is this value */
  int *pN,                 /* OUT: Write number of blobs loaded here */
  Blob **ppList            /* OUT: Write the head of the blob list here */
){
  Blob head;
  Blob *p;
  sqlite3_stmt *pStmt;
  int n = 0;
  int rc;
  char *z2;

  if( onlyId>0 ){
    z2 = sqlite3_mprintf("%s WHERE rowid=%d", zSql, onlyId);
  }else{
    z2 = sqlite3_mprintf("%s", zSql);
  }
  rc = sqlite3_prepare_v2(db, z2, -1, &pStmt, 0);
  sqlite3_free(z2);
  if( rc ) fatalError("%s", sqlite3_errmsg(db));
  head.pNext = 0;
  p = &head;
  while( SQLITE_ROW==sqlite3_step(pStmt) ){
    int sz = sqlite3_column_bytes(pStmt, 1);
    Blob *pNew = safe_realloc(0, sizeof(*pNew)+sz );
    pNew->id = sqlite3_column_int(pStmt, 0);
    pNew->sz = sz;
    pNew->seq = n++;
    pNew->pNext = 0;
    memcpy(pNew->a, sqlite3_column_blob(pStmt,1), sz);
    pNew->a[sz] = 0;
    p->pNext = pNew;
    p = pNew;
  }
  sqlite3_finalize(pStmt);
  *pN = n;
  *ppList = head.pNext;
}

/*
** Free a list of Blob objects
*/
static void blobListFree(Blob *p){
  Blob *pNext;
  while( p ){
    pNext = p->pNext;
    free(p);
    p = pNext;
  }
}


/* Return the current wall-clock time */
static sqlite3_int64 timeOfDay(void){
  static sqlite3_vfs *clockVfs = 0;
  sqlite3_int64 t;
  if( clockVfs==0 ) clockVfs = sqlite3_vfs_find(0);
  if( clockVfs->iVersion>=1 && clockVfs->xCurrentTimeInt64!=0 ){
    clockVfs->xCurrentTimeInt64(clockVfs, &t);
  }else{
    double r;
    clockVfs->xCurrentTime(clockVfs, &r);
    t = (sqlite3_int64)(r*86400000.0);
  }
  return t;
}

/* Methods for the VHandle object
*/
static int inmemClose(sqlite3_file *pFile){
  VHandle *p = (VHandle*)pFile;
  VFile *pVFile = p->pVFile;
  pVFile->nRef--;
  if( pVFile->nRef==0 && pVFile->zFilename==0 ){
    pVFile->sz = -1;
    free(pVFile->a);
    pVFile->a = 0;
  }
  return SQLITE_OK;
}
static int inmemRead(
  sqlite3_file *pFile,   /* Read from this open file */
  void *pData,           /* Store content in this buffer */
  int iAmt,              /* Bytes of content */
  sqlite3_int64 iOfst    /* Start reading here */
){
  VHandle *pHandle = (VHandle*)pFile;
  VFile *pVFile = pHandle->pVFile;
  if( iOfst<0 || iOfst>=pVFile->sz ){
    memset(pData, 0, iAmt);
    return SQLITE_IOERR_SHORT_READ;
  }
  if( iOfst+iAmt>pVFile->sz ){
    memset(pData, 0, iAmt);
    iAmt = (int)(pVFile->sz - iOfst);
    memcpy(pData, pVFile->a, iAmt);
    return SQLITE_IOERR_SHORT_READ;
  }
  memcpy(pData, pVFile->a + iOfst, iAmt);
  return SQLITE_OK;
}
static int inmemWrite(
  sqlite3_file *pFile,   /* Write to this file */
  const void *pData,     /* Content to write */
  int iAmt,              /* bytes to write */
  sqlite3_int64 iOfst    /* Start writing here */
){
  VHandle *pHandle = (VHandle*)pFile;
  VFile *pVFile = pHandle->pVFile;
  if( iOfst+iAmt > pVFile->sz ){
    if( iOfst+iAmt >= MX_FILE_SZ ){
      return SQLITE_FULL;
    }
    pVFile->a = safe_realloc(pVFile->a, (int)(iOfst+iAmt));
    if( iOfst > pVFile->sz ){
      memset(pVFile->a + pVFile->sz, 0, (int)(iOfst - pVFile->sz));
    }
    pVFile->sz = (int)(iOfst + iAmt);
  }
  memcpy(pVFile->a + iOfst, pData, iAmt);
  return SQLITE_OK;
}
static int inmemTruncate(sqlite3_file *pFile, sqlite3_int64 iSize){
  VHandle *pHandle = (VHandle*)pFile;
  VFile *pVFile = pHandle->pVFile;
  if( pVFile->sz>iSize && iSize>=0 ) pVFile->sz = (int)iSize;
  return SQLITE_OK;
}
static int inmemSync(sqlite3_file *pFile, int flags){
  return SQLITE_OK;
}
static int inmemFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize){
  *pSize = ((VHandle*)pFile)->pVFile->sz;
  return SQLITE_OK;
}
static int inmemLock(sqlite3_file *pFile, int type){
  return SQLITE_OK;
}
static int inmemUnlock(sqlite3_file *pFile, int type){
  return SQLITE_OK;
}
static int inmemCheckReservedLock(sqlite3_file *pFile, int *pOut){
  *pOut = 0;
  return SQLITE_OK;
}
static int inmemFileControl(sqlite3_file *pFile, int op, void *pArg){
  return SQLITE_NOTFOUND;
}
static int inmemSectorSize(sqlite3_file *pFile){
  return 512;
}
static int inmemDeviceCharacteristics(sqlite3_file *pFile){
  return
      SQLITE_IOCAP_SAFE_APPEND |
      SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN |
      SQLITE_IOCAP_POWERSAFE_OVERWRITE;
}


/* Method table for VHandle
*/
static sqlite3_io_methods VHandleMethods = {
  /* iVersion  */    1,
  /* xClose    */    inmemClose,
  /* xRead     */    inmemRead,
  /* xWrite    */    inmemWrite,
  /* xTruncate */    inmemTruncate,
  /* xSync     */    inmemSync,
  /* xFileSize */    inmemFileSize,
  /* xLock     */    inmemLock,
  /* xUnlock   */    inmemUnlock,
  /* xCheck... */    inmemCheckReservedLock,
  /* xFileCtrl */    inmemFileControl,
  /* xSectorSz */    inmemSectorSize,
  /* xDevchar  */    inmemDeviceCharacteristics,
  /* xShmMap   */    0,
  /* xShmLock  */    0,
  /* xShmBarrier */  0,
  /* xShmUnmap */    0,
  /* xFetch    */    0,
  /* xUnfetch  */    0
};

/*
** Open a new file in the inmem VFS.  All files are anonymous and are
** delete-on-close.
*/
static int inmemOpen(
  sqlite3_vfs *pVfs,
  const char *zFilename,
  sqlite3_file *pFile,
  int openFlags,
  int *pOutFlags
){
  VFile *pVFile = createVFile(zFilename, 0, (unsigned char*)"");
  VHandle *pHandle = (VHandle*)pFile;
  if( pVFile==0 ){
    return SQLITE_FULL;
  }
  pHandle->pVFile = pVFile;
  pVFile->nRef++;
  pFile->pMethods = &VHandleMethods;
  if( pOutFlags ) *pOutFlags = openFlags;
  return SQLITE_OK;
}

/*
** Delete a file by name
*/
static int inmemDelete(
  sqlite3_vfs *pVfs,
  const char *zFilename,
  int syncdir
){
  VFile *pVFile = findVFile(zFilename);
  if( pVFile==0 ) return SQLITE_OK;
  if( pVFile->nRef==0 ){
    free(pVFile->zFilename);
    pVFile->zFilename = 0;
    pVFile->sz = -1;
    free(pVFile->a);
    pVFile->a = 0;
    return SQLITE_OK;
  }
  return SQLITE_IOERR_DELETE;
}

/* Check for the existance of a file
*/
static int inmemAccess(
  sqlite3_vfs *pVfs,
  const char *zFilename,
  int flags,
  int *pResOut
){
  VFile *pVFile = findVFile(zFilename);
  *pResOut =  pVFile!=0;
  return SQLITE_OK;
}

/* Get the canonical pathname for a file
*/
static int inmemFullPathname(
  sqlite3_vfs *pVfs,
  const char *zFilename,
  int nOut,
  char *zOut
){
  sqlite3_snprintf(nOut, zOut, "%s", zFilename);
  return SQLITE_OK;
}

/* GetLastError() is never used */
static int inmemGetLastError(sqlite3_vfs *pVfs, int n, char *z){
  return SQLITE_OK;
}

/*
** Register the VFS that reads from the g.aFile[] set of files.
*/
static void inmemVfsRegister(void){
  static sqlite3_vfs inmemVfs;
  sqlite3_vfs *pDefault = sqlite3_vfs_find(0);
  inmemVfs.iVersion = 1;
  inmemVfs.szOsFile = sizeof(VHandle);
  inmemVfs.mxPathname = 200;
  inmemVfs.zName = "inmem";
  inmemVfs.xOpen = inmemOpen;
  inmemVfs.xDelete = inmemDelete;
  inmemVfs.xAccess = inmemAccess;
  inmemVfs.xFullPathname = inmemFullPathname;
  inmemVfs.xRandomness = pDefault->xRandomness;
  inmemVfs.xSleep = pDefault->xSleep;
  inmemVfs.xCurrentTime = pDefault->xCurrentTime;
  inmemVfs.xGetLastError = inmemGetLastError;
  sqlite3_vfs_register(&inmemVfs, 0);
};

/*
** Allowed values for the runFlags parameter to runSql()
*/
#define SQL_TRACE  0x0001     /* Print each SQL statement as it is prepared */
#define SQL_OUTPUT 0x0002     /* Show the SQL output */

/*
** Run multiple commands of SQL.  Similar to sqlite3_exec(), but does not
** stop if an error is encountered.
*/
static void runSql(sqlite3 *db, const char *zSql, unsigned  runFlags){
  const char *zMore;
  sqlite3_stmt *pStmt;

  while( zSql && zSql[0] ){
    zMore = 0;
    pStmt = 0;
    sqlite3_prepare_v2(db, zSql, -1, &pStmt, &zMore);
    if( zMore==zSql ) break;
    if( runFlags & SQL_TRACE ){
      const char *z = zSql;
      int n;
      while( z<zMore && isspace(z[0]) ) z++;
      n = (int)(zMore - z);
      while( n>0 && isspace(z[n-1]) ) n--;
      if( n==0 ) break;
      if( pStmt==0 ){
        printf("TRACE: %.*s (error: %s)\n", n, z, sqlite3_errmsg(db));
      }else{
        printf("TRACE: %.*s\n", n, z);
      }
    }
    zSql = zMore;
    if( pStmt ){
      if( (runFlags & SQL_OUTPUT)==0 ){
        while( SQLITE_ROW==sqlite3_step(pStmt) ){}
      }else{
        int nCol = -1;
        while( SQLITE_ROW==sqlite3_step(pStmt) ){
          int i;
          if( nCol<0 ){
            nCol = sqlite3_column_count(pStmt);
          }else if( nCol>0 ){
            printf("--------------------------------------------\n");
          }
          for(i=0; i<nCol; i++){
            int eType = sqlite3_column_type(pStmt,i);
            printf("%s = ", sqlite3_column_name(pStmt,i));
            switch( eType ){
              case SQLITE_NULL: {
                printf("NULL\n");
                break;
              }
              case SQLITE_INTEGER: {
                printf("INT %s\n", sqlite3_column_text(pStmt,i));
                break;
              }
              case SQLITE_FLOAT: {
                printf("FLOAT %s\n", sqlite3_column_text(pStmt,i));
                break;
              }
              case SQLITE_TEXT: {
                printf("TEXT [%s]\n", sqlite3_column_text(pStmt,i));
                break;
              }
              case SQLITE_BLOB: {
                printf("BLOB (%d bytes)\n", sqlite3_column_bytes(pStmt,i));
                break;
              }
            }
          }
        }
      }         
      sqlite3_finalize(pStmt);
    }
  }
}

/*
** Rebuild the database file.
**
**    (1)  Remove duplicate entries
**    (2)  Put all entries in order
**    (3)  Vacuum
*/
static void rebuild_database(sqlite3 *db){
  int rc;
  rc = sqlite3_exec(db, 
     "BEGIN;\n"
     "CREATE TEMP TABLE dbx AS SELECT DISTINCT dbcontent FROM db;\n"
     "DELETE FROM db;\n"
     "INSERT INTO db(dbid, dbcontent) SELECT NULL, dbcontent FROM dbx ORDER BY 2;\n"
     "DROP TABLE dbx;\n"
     "CREATE TEMP TABLE sx AS SELECT DISTINCT sqltext FROM xsql;\n"
     "DELETE FROM xsql;\n"
     "INSERT INTO xsql(sqlid,sqltext) SELECT NULL, sqltext FROM sx ORDER BY 2;\n"
     "DROP TABLE sx;\n"
     "COMMIT;\n"
     "PRAGMA page_size=1024;\n"
     "VACUUM;\n", 0, 0, 0);
  if( rc ) fatalError("cannot rebuild: %s", sqlite3_errmsg(db));
}

/*
** Print sketchy documentation for this utility program
*/
static void showHelp(void){
  printf("Usage: %s [options] SOURCE-DB ?ARGS...?\n", g.zArgv0);
  printf(
"Read databases and SQL scripts from SOURCE-DB and execute each script against\n"
"each database, checking for crashes and memory leaks.\n"
"Options:\n"
"  --cell-size-check     Set the PRAGMA cell_size_check=ON\n"
"  --dbid N              Use only the database where dbid=N\n"
"  --help                Show this help text\n"
"  -q                    Reduced output\n"
"  --quiet               Reduced output\n"
"  --limit-vdbe          Panic if an sync SQL runs for more than 100,000 cycles\n"
"  --load-sql ARGS...    Load SQL scripts fro files into SOURCE-DB\n"
"  --load-db ARGS...     Load template databases from files into SOURCE_DB\n"
"  -m TEXT               Add a description to the database\n"
"  --native-vfs          Use the native VFS for initially empty database files\n"
"  --rebuild             Rebuild and vacuum the database file\n"
"  --result-trace        Show the results of each SQL command\n"
"  --sqlid N             Use only SQL where sqlid=N\n"
"  -v                    Increased output\n"
"  --verbose             Increased output\n"
  );
}

int main(int argc, char **argv){
  sqlite3_int64 iBegin;        /* Start time of this program */
  int quietFlag = 0;           /* True if --quiet or -q */
  int verboseFlag = 0;         /* True if --verbose or -v */
  char *zInsSql = 0;           /* SQL statement for --load-db or --load-sql */
  int iFirstInsArg = 0;        /* First argv[] to use for --load-db or --load-sql */
  sqlite3 *db = 0;             /* The open database connection */
  sqlite3_stmt *pStmt;         /* A prepared statement */
  int rc;                      /* Result code from SQLite interface calls */
  Blob *pSql;                  /* For looping over SQL scripts */
  Blob *pDb;                   /* For looping over template databases */
  int i;                       /* Loop index for the argv[] loop */
  int onlySqlid = -1;          /* --sqlid */
  int onlyDbid = -1;           /* --dbid */
  int nativeFlag = 0;          /* --native-vfs */
  int rebuildFlag = 0;         /* --rebuild */
  int vdbeLimitFlag = 0;       /* --limit-vdbe */
  int timeoutTest = 0;         /* undocumented --timeout-test flag */
  int runFlags = 0;            /* Flags sent to runSql() */
  char *zMsg = 0;              /* Add this message */
  int nSrcDb = 0;              /* Number of source databases */
  char **azSrcDb = 0;          /* Array of source database names */
  int iSrcDb;                  /* Loop over all source databases */
  int nTest = 0;               /* Total number of tests performed */
  char *zDbName = "";          /* Appreviated name of a source database */
  const char *zFailCode = 0;   /* Value of the TEST_FAILURE environment variable */
  int cellSzCkFlag = 0;        /* --cell-size-check */
  int sqlFuzz = 0;             /* True for SQL fuzz testing. False for DB fuzz */

  iBegin = timeOfDay();
#ifdef __unix__
  signal(SIGALRM, timeoutHandler);
#endif
  g.zArgv0 = argv[0];
  zFailCode = getenv("TEST_FAILURE");
  for(i=1; i<argc; i++){
    const char *z = argv[i];
    if( z[0]=='-' ){
      z++;
      if( z[0]=='-' ) z++;
      if( strcmp(z,"cell-size-check")==0 ){
        cellSzCkFlag = 1;
      }else
      if( strcmp(z,"dbid")==0 ){
        if( i>=argc-1 ) fatalError("missing arguments on %s", argv[i]);
        onlyDbid = atoi(argv[++i]);
      }else
      if( strcmp(z,"help")==0 ){
        showHelp();
        return 0;
      }else
      if( strcmp(z,"limit-vdbe")==0 ){
        vdbeLimitFlag = 1;
      }else
      if( strcmp(z,"load-sql")==0 ){
        zInsSql = "INSERT INTO xsql(sqltext) VALUES(CAST(readfile(?1) AS text))";
        iFirstInsArg = i+1;
        break;
      }else
      if( strcmp(z,"load-db")==0 ){
        zInsSql = "INSERT INTO db(dbcontent) VALUES(readfile(?1))";
        iFirstInsArg = i+1;
        break;
      }else
      if( strcmp(z,"m")==0 ){
        if( i>=argc-1 ) fatalError("missing arguments on %s", argv[i]);
        zMsg = argv[++i];
      }else
      if( strcmp(z,"native-vfs")==0 ){
        nativeFlag = 1;
      }else
      if( strcmp(z,"quiet")==0 || strcmp(z,"q")==0 ){
        quietFlag = 1;
        verboseFlag = 0;
      }else
      if( strcmp(z,"rebuild")==0 ){
        rebuildFlag = 1;
      }else
      if( strcmp(z,"result-trace")==0 ){
        runFlags |= SQL_OUTPUT;
      }else
      if( strcmp(z,"sqlid")==0 ){
        if( i>=argc-1 ) fatalError("missing arguments on %s", argv[i]);
        onlySqlid = atoi(argv[++i]);
      }else
      if( strcmp(z,"timeout-test")==0 ){
        timeoutTest = 1;
#ifndef __unix__
        fatalError("timeout is not available on non-unix systems");
#endif
      }else
      if( strcmp(z,"verbose")==0 || strcmp(z,"v")==0 ){
        quietFlag = 0;
        verboseFlag = 1;
        runFlags |= SQL_TRACE;
      }else
      {
        fatalError("unknown option: %s", argv[i]);
      }
    }else{
      nSrcDb++;
      azSrcDb = safe_realloc(azSrcDb, nSrcDb*sizeof(azSrcDb[0]));
      azSrcDb[nSrcDb-1] = argv[i];
    }
  }
  if( nSrcDb==0 ) fatalError("no source database specified");
  if( nSrcDb>1 ){
    if( zMsg ){
      fatalError("cannot change the description of more than one database");
    }
    if( zInsSql ){
      fatalError("cannot import into more than one database");
    }
  }

  /* Process each source database separately */
  for(iSrcDb=0; iSrcDb<nSrcDb; iSrcDb++){
    rc = sqlite3_open(azSrcDb[iSrcDb], &db);
    if( rc ){
      fatalError("cannot open source database %s - %s",
      azSrcDb[iSrcDb], sqlite3_errmsg(db));
    }
    rc = sqlite3_exec(db,
       "CREATE TABLE IF NOT EXISTS db(\n"
       "  dbid INTEGER PRIMARY KEY, -- database id\n"
       "  dbcontent BLOB            -- database disk file image\n"
       ");\n"
       "CREATE TABLE IF NOT EXISTS xsql(\n"
       "  sqlid INTEGER PRIMARY KEY,   -- SQL script id\n"
       "  sqltext TEXT                 -- Text of SQL statements to run\n"
       ");"
       "CREATE TABLE IF NOT EXISTS readme(\n"
       "  msg TEXT -- Human-readable description of this file\n"
       ");", 0, 0, 0);
    if( rc ) fatalError("cannot create schema: %s", sqlite3_errmsg(db));
    if( zMsg ){
      char *zSql;
      zSql = sqlite3_mprintf(
               "DELETE FROM readme; INSERT INTO readme(msg) VALUES(%Q)", zMsg);
      rc = sqlite3_exec(db, zSql, 0, 0, 0);
      sqlite3_free(zSql);
      if( rc ) fatalError("cannot change description: %s", sqlite3_errmsg(db));
    }
    if( zInsSql ){
      sqlite3_create_function(db, "readfile", 1, SQLITE_UTF8, 0,
                              readfileFunc, 0, 0);
      rc = sqlite3_prepare_v2(db, zInsSql, -1, &pStmt, 0);
      if( rc ) fatalError("cannot prepare statement [%s]: %s",
                          zInsSql, sqlite3_errmsg(db));
      rc = sqlite3_exec(db, "BEGIN", 0, 0, 0);
      if( rc ) fatalError("cannot start a transaction");
      for(i=iFirstInsArg; i<argc; i++){
        sqlite3_bind_text(pStmt, 1, argv[i], -1, SQLITE_STATIC);
        sqlite3_step(pStmt);
        rc = sqlite3_reset(pStmt);
        if( rc ) fatalError("insert failed for %s", argv[i]);
      }
      sqlite3_finalize(pStmt);
      rc = sqlite3_exec(db, "COMMIT", 0, 0, 0);
      if( rc ) fatalError("cannot commit the transaction: %s", sqlite3_errmsg(db));
      rebuild_database(db);
      sqlite3_close(db);
      return 0;
    }
  
    /* Load all SQL script content and all initial database images from the
    ** source db
    */
    blobListLoadFromDb(db, "SELECT sqlid, sqltext FROM xsql", onlySqlid,
                           &g.nSql, &g.pFirstSql);
    if( g.nSql==0 ) fatalError("need at least one SQL script");
    blobListLoadFromDb(db, "SELECT dbid, dbcontent FROM db", onlyDbid,
                       &g.nDb, &g.pFirstDb);
    if( g.nDb==0 ){
      g.pFirstDb = safe_realloc(0, sizeof(Blob));
      memset(g.pFirstDb, 0, sizeof(Blob));
      g.pFirstDb->id = 1;
      g.pFirstDb->seq = 0;
      g.nDb = 1;
      sqlFuzz = 1;
    }
  
    /* Print the description, if there is one */
    if( !quietFlag ){
      int i;
      zDbName = azSrcDb[iSrcDb];
      i = strlen(zDbName) - 1;
      while( i>0 && zDbName[i-1]!='/' && zDbName[i-1]!='\\' ){ i--; }
      zDbName += i;
      sqlite3_prepare_v2(db, "SELECT msg FROM readme", -1, &pStmt, 0);
      if( pStmt && sqlite3_step(pStmt)==SQLITE_ROW ){
        printf("%s: %s\n", zDbName, sqlite3_column_text(pStmt,0));
      }
      sqlite3_finalize(pStmt);
    }

    /* Rebuild the database, if requested */
    if( rebuildFlag ){
      if( !quietFlag ){
        printf("%s: rebuilding... ", zDbName);
        fflush(stdout);
      }
      rebuild_database(db);
      if( !quietFlag ) printf("done\n");
    }
  
    /* Close the source database.  Verify that no SQLite memory allocations are
    ** outstanding.
    */
    sqlite3_close(db);
    if( sqlite3_memory_used()>0 ){
      fatalError("SQLite has memory in use before the start of testing");
    }
  
    /* Register the in-memory virtual filesystem
    */
    formatVfs();
    inmemVfsRegister();
    
    /* Run a test using each SQL script against each database.
    */
    if( !verboseFlag && !quietFlag ) printf("%s:", zDbName);
    for(pSql=g.pFirstSql; pSql; pSql=pSql->pNext){
      for(pDb=g.pFirstDb; pDb; pDb=pDb->pNext){
        int openFlags;
        const char *zVfs = "inmem";
        sqlite3_snprintf(sizeof(g.zTestName), g.zTestName, "sqlid=%d,dbid=%d",
                         pSql->id, pDb->id);
        if( verboseFlag ){
          printf("%s\n", g.zTestName);
          fflush(stdout);
        }else if( !quietFlag ){
          static int prevAmt = -1;
          int idx = pSql->seq*g.nDb + pDb->id - 1;
          int amt = idx*10/(g.nDb*g.nSql);
          if( amt!=prevAmt ){
            printf(" %d%%", amt*10);
            fflush(stdout);
            prevAmt = amt;
          }
        }
        createVFile("main.db", pDb->sz, pDb->a);
        openFlags = SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE;
        if( nativeFlag && pDb->sz==0 ){
          openFlags |= SQLITE_OPEN_MEMORY;
          zVfs = 0;
        }
        rc = sqlite3_open_v2("main.db", &db, openFlags, zVfs);
        if( rc ) fatalError("cannot open inmem database");
        if( cellSzCkFlag ) runSql(db, "PRAGMA cell_size_check=ON", runFlags);
        setAlarm(10);
        if( sqlFuzz || vdbeLimitFlag ){
          sqlite3_progress_handler(db, 100000, progressHandler, &vdbeLimitFlag);
        }
        do{
          runSql(db, (char*)pSql->a, runFlags);
        }while( timeoutTest );
        setAlarm(0);
        sqlite3_close(db);
        if( sqlite3_memory_used()>0 ) fatalError("memory leak");
        reformatVfs();
        nTest++;
        g.zTestName[0] = 0;

        /* Simulate an error if the TEST_FAILURE environment variable is "5".
        ** This is used to verify that automated test script really do spot
        ** errors that occur in this test program.
        */
        if( zFailCode ){
          if( zFailCode[0]=='5' && zFailCode[1]==0 ){
            fatalError("simulated failure");
          }else if( zFailCode[0]!=0 ){
            /* If TEST_FAILURE is something other than 5, just exit the test
            ** early */
            printf("\nExit early due to TEST_FAILURE being set\n");
            iSrcDb = nSrcDb-1;
            goto sourcedb_cleanup;
          }
        }
      }
    }
    if( !quietFlag && !verboseFlag ){
      printf(" 100%% - %d tests\n", g.nDb*g.nSql);
    }
  
    /* Clean up at the end of processing a single source database
    */
  sourcedb_cleanup:
    blobListFree(g.pFirstSql);
    blobListFree(g.pFirstDb);
    reformatVfs();
 
  } /* End loop over all source databases */

  if( !quietFlag ){
    sqlite3_int64 iElapse = timeOfDay() - iBegin;
    printf("fuzzcheck: 0 errors out of %d tests in %d.%03d seconds\n"
           "SQLite %s %s\n",
           nTest, (int)(iElapse/1000), (int)(iElapse%1000),
           sqlite3_libversion(), sqlite3_sourceid());
  }
  free(azSrcDb);
  return 0;
}
