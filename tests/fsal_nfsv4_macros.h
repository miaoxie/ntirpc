/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

/**
 * \file    fsal_nfsv4_macros.h
 * \author  $Author: deniel $
 * \date    06/05/2007
 * \version $Revision$
 * \brief   Usefull macros to manage NFSv4 call from FSAL_PROXY
 *
 *
 */
#ifndef _FSAL_NFSV4_MACROS_H
#define _FSAL_NFSV4_MACROS_H


#define MAXNAMLEN 256

typedef union {
  nfs_cookie4 data ;
  char pad[4];

} proxyfsal_cookie_t;

typedef struct fsal_proxy_internal_fattr_readdir__
{
  fattr4_type type;
  fattr4_change change_time;
  fattr4_size size;
  fattr4_fsid fsid;
  fattr4_filehandle filehandle;
  fattr4_fileid fileid;
  fattr4_mode mode;
  fattr4_numlinks numlinks;
  fattr4_owner owner;           /* Needs to points to a string */
  fattr4_owner_group owner_group;       /* Needs to points to a string */
  fattr4_space_used space_used;
  fattr4_time_access time_access;
  fattr4_time_metadata time_metadata;
  fattr4_time_modify time_modify;
  fattr4_rawdev rawdev;
  char padowner[MAXNAMLEN];
  char padgroup[MAXNAMLEN];
  char padfh[NFS4_FHSIZE];
} fsal_proxy_internal_fattr_readdir_t;



#define TIMEOUTRPC {2, 0} 

#define PRINT_HANDLE( tag, handle )                                                     \
  do {                                                                                  \
    if(isFullDebug(COMPONENT_FSAL))                                                     \
      {                                                                                 \
        char outstr[1024] ;                                                             \
        snprintHandle(outstr, 1024, handle) ;                                           \
        LogFullDebug(COMPONENT_FSAL, "============> %s : handle=%s\n", tag, outstr ) ;  \
      }                                                                                 \
  } while( 0 )

/* Free a compound */
#define COMPOUNDV4_ARG_FREE \
do {gsh_free(argcompound.argarray_val);} while( 0 )

/* OP specific macros */
#define COMPOUNDV4_ARG_ADD_OP_PUTROOTFH( argcompound )                                             \
do {                                                                                               \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_PUTROOTFH ; \
  argcompound.argarray.argarray_len += 1 ;                                                         \
} while( 0 )

#define COMPOUNDV4_ARG_ADD_OP_OPEN_CONFIRM( argcompound, __openseqid, __other, __seqid )                                                       \
do {                                                                                                                                           \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_OPEN_CONFIRM ;                                          \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen_confirm.seqid = __seqid ;                           \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen_confirm.open_stateid.seqid = __openseqid ;          \
  memcpy( argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen_confirm.open_stateid.other, __other, 12 ) ; \
  argcompound.argarray.argarray_len += 1 ;                                                                                                     \
} while( 0 )

#define COMPOUNDV4_ARG_ADD_OP_OPEN_NOCREATE( argcompound, __seqid, inclientid, inaccess, inname, __owner_val, __owner_len )             \
do {                                                                                                                                    \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_OPEN ;                                           \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.seqid = __seqid ;                            \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.share_access = OPEN4_SHARE_ACCESS_BOTH ;     \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.share_deny = OPEN4_SHARE_DENY_NONE ;         \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.owner.clientid = inclientid ;                \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.owner.owner.owner_len =  __owner_len  ;      \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.owner.owner.owner_val = __owner_val ;        \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.openhow.opentype = OPEN4_NOCREATE ;          \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.claim.claim = CLAIM_NULL ;                   \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.claim.open_claim4_u.file = inname ;          \
  argcompound.argarray.argarray_len += 1 ;                                                                                              \
} while( 0 )

#define COMPOUNDV4_ARG_ADD_OP_CLOSE( argcompound, __stateid )                                                                                    \
do {                                                                                                                                             \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_CLOSE ;                                                   \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opclose.seqid = __stateid.seqid +1 ;                         \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opclose.open_stateid.seqid = __stateid.seqid ;               \
  memcpy( argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opclose.open_stateid.other, __stateid.other, 12 ) ;  \
  argcompound.argarray.argarray_len += 1 ;                                                                                                       \
} while( 0 )

#define COMPOUNDV4_ARG_ADD_OP_GETATTR( argcompound, bitmap )                                                          \
do {                                                                                                                  \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_GETATTR ;                      \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opgetattr.attr_request = bitmap ; \
  argcompound.argarray.argarray_len += 1 ;                                                                            \
} while( 0 )

#define COMPOUNDV4_ARG_ADD_OP_SETATTR( argcompound, inattr )                                                                       \
do {                                                                                                                               \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_SETATTR ;                                   \
  memset(&argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opsetattr.stateid,0,sizeof(stateid4)); \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opsetattr.obj_attributes = inattr ;            \
  argcompound.argarray.argarray_len += 1 ;                                                                                         \
} while( 0 )

#define COMPOUNDV4_ARG_ADD_OP_GETFH( argcompound )                                                            \
do {                                                                                                          \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_GETFH ;                \
  argcompound.argarray.argarray_len += 1 ;                                                                    \
} while( 0 )

#define COMPOUNDV4_ARG_ADD_OP_PUTFH( argcompound, nfs4fh )                                                    \
do {                                                                                                          \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_PUTFH ;                \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opputfh.object = nfs4fh ; \
  argcompound.argarray.argarray_len += 1 ;                                                                    \
} while( 0 )

#define COMPOUNDV4_ARG_ADD_OP_LOOKUP( argcompound, name )                                                     \
do {                                                                                                          \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_LOOKUP ;               \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.oplookup.objname = name ; \
  argcompound.argarray.argarray_len += 1 ;                                                                    \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_LOOKUPP( argcompound )                                             \
do {                                                                                             \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_LOOKUPP ; \
  argcompound.argarray.argarray_len += 1 ;                                                       \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_SETCLIENTID( argcompound, inclient, incallback )                                            \
do {                                                                                                                      \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_SETCLIENTID ;                      \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opsetclientid.client = inclient ;     \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opsetclientid.callback = incallback ; \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opsetclientid.callback_ident = 0 ;    \
  argcompound.argarray.argarray_len += 1 ;                                                                                \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_SETCLIENTID_CONFIRM( argcompound, inclientid, inverifier )                                                                                   \
do {                                                                                                                                                                       \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_SETCLIENTID_CONFIRM ;                                                               \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opsetclientid_confirm.clientid = inclientid ;                                          \
  strncpy( argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opsetclientid_confirm.setclientid_confirm, inverifier, NFS4_VERIFIER_SIZE ) ; \
  argcompound.argarray.argarray_len += 1 ;                                                                                                                                 \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_ACCESS( argcompound, inaccessflag )                                                    \
do {                                                                                                                 \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_ACCESS ;                      \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opaccess.access = inaccessflag ; \
  argcompound.argarray.argarray_len += 1 ;                                                                           \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_READDIR( argcompound, incookie, innbentry, inverifier, inbitmap  )                                                     \
do {                                                                                                                                                 \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_READDIR ;                                                     \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opreaddir.cookie       = incookie ;                              \
  memcpy( argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opreaddir.cookieverf, inverifier, NFS4_VERIFIER_SIZE ) ; \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opreaddir.dircount     = innbentry*sizeof( entry4 ) ;            \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opreaddir.dircount     = 2048 ;                                  \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opreaddir.maxcount     = innbentry*sizeof( entry4 ) ;            \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opreaddir.maxcount     = 4096 ;                                  \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opreaddir.attr_request = inbitmap ;                              \
  argcompound.argarray.argarray_len += 1 ;                                                                                                           \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_OPEN_CREATE( argcompound, inname, inattrs, inclientid, __owner_val, __owner_len )                                         \
do {                                                                                                                                                    \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_OPEN ;                                                           \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.seqid = 0 ;                                                  \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.share_access = OPEN4_SHARE_ACCESS_BOTH ;                     \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.share_deny = OPEN4_SHARE_DENY_NONE ;                         \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.owner.clientid = inclientid ;                                \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.owner.owner.owner_len =  __owner_len ;                       \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.owner.owner.owner_val =  __owner_val ;                       \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.openhow.opentype = OPEN4_CREATE ;                            \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.openhow.openflag4_u.how.mode = GUARDED4 ;                    \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.openhow.openflag4_u.how.createhow4_u.createattrs = inattrs ; \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.claim.claim = CLAIM_NULL ;                                   \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opopen.claim.open_claim4_u.file = inname ;                          \
  argcompound.argarray.argarray_len += 1 ;                                                                                                              \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_MKDIR( argcompound, inname, inattrs )                                                  \
do {                                                                                                                 \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_CREATE ;                      \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opcreate.objtype.type = NF4DIR ; \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opcreate.objname = inname ;      \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opcreate.createattrs = inattrs ; \
  argcompound.argarray.argarray_len += 1 ;                                                                           \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_SYMLINK( argcompound, inname, incontent, inattrs )                                                          \
do {                                                                                                                                      \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_CREATE ;                                           \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opcreate.objtype.type = NF4LNK ;                      \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opcreate.objtype.createtype4_u.linkdata = incontent ; \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opcreate.objname = inname ;                           \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opcreate.createattrs = inattrs ;                      \
  argcompound.argarray.argarray_len += 1 ;                                                                                                \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_LINK( argcompound, inname )                                                     \
do {                                                                                                          \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_LINK ;                 \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.oplink.newname = inname ; \
  argcompound.argarray.argarray_len += 1 ;                                                                    \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_REMOVE( argcompound, inname )                                                    \
do {                                                                                                           \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_REMOVE ;                \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opremove.target = inname ; \
  argcompound.argarray.argarray_len += 1 ;                                                                     \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_RENAME( argcompound, inoldname, innewname )                                          \
do {                                                                                                               \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_RENAME ;                    \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.oprename.oldname = inoldname ; \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.oprename.newname = innewname ; \
  argcompound.argarray.argarray_len += 1 ;                                                                         \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_READLINK( argcompound )                                             \
do {                                                                                              \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_READLINK ; \
  argcompound.argarray.argarray_len += 1 ;                                                        \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_SAVEFH( argcompound )                                             \
do {                                                                                            \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_SAVEFH ; \
  argcompound.argarray.argarray_len += 1 ;                                                      \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_RESTOREFH( argcompound )                                             \
do {                                                                                               \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_RESTOREFH ; \
  argcompound.argarray.argarray_len += 1 ;                                                         \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_READ( argcompound, instateid, inoffset, incount )                                                                \
do {                                                                                                                                           \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_READ ;                                                  \
  memcpy( &argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opread.stateid, instateid, sizeof( stateid4 ) ) ; \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opread.offset = inoffset ;                                 \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opread.count  = incount ;                                  \
  argcompound.argarray.argarray_len += 1 ;                                                                                                     \
} while ( 0 )

#define COMPOUNDV4_ARG_ADD_OP_WRITE( argcompound, instateid, inoffset, indatabuffval, indatabufflen )                                           \
do {                                                                                                                                            \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].argop = OP_WRITE ;                                                  \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opwrite.stable= DATA_SYNC4 ;                                \
  memcpy( &argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opwrite.stateid, instateid, sizeof( stateid4 ) ) ; \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opwrite.offset = inoffset ;                                 \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opwrite.data.data_val = indatabuffval ;                     \
  argcompound.argarray.argarray_val[argcompound.argarray.argarray_len].nfs_argop4_u.opwrite.data.data_len = indatabufflen ;                     \
  argcompound.argarray.argarray_len += 1 ;                                                                                                      \
} while ( 0 )

#define COMPOUNDV4_EXECUTE( pcontext, argcompound, rescompound, rc )                      \
do {                                                                                      \
  int __renew_rc = 0 ;                                                                    \
  rc = -1 ;                                                                               \
  do {                                                                                    \
  if( __renew_rc == 0 )                                                                   \
      {                                                                                   \
        if( FSAL_proxy_change_user( pcontext ) == NULL ) break  ;                         \
        if( ( rc = clnt_call( pcontext->rpc_client, NFSPROC4_COMPOUND,                    \
                              (xdrproc_t)xdr_COMPOUND4args, (caddr_t)&argcompound,        \
                              (xdrproc_t)xdr_COMPOUND4res,  (caddr_t)&rescompound,        \
                              timeout ) ) == RPC_SUCCESS )                                \
              break ;                                                                     \
       }                                                                                  \
  LogEvent(COMPONENT_FSAL, "Reconnecting to the remote server.." ) ;                      \
  pthread_mutex_lock( &pcontext->lock ) ;                                                 \
  __renew_rc = fsal_internal_ClientReconnect( pcontext ) ;                                \
  pthread_mutex_unlock( &pcontext->lock ) ;                                               \
  if (__renew_rc) {                                                                       \
     LogEvent(COMPONENT_FSAL, "Cannot reconnect, will sleep for %d seconds",              \
              pcontext->retry_sleeptime ) ;                                               \
   sleep( pcontext->retry_sleeptime ) ;                                                   \
  }                                                                                       \
  } while( 1  ) ;                                                                         \
}  while( 0 )

#define COMPOUNDV4_EXECUTE_SIMPLE( pcontext, argcompound, rescompound )   \
   clnt_call( pcontext->rpc_client, NFSPROC4_COMPOUND,                    \
              (xdrproc_t)xdr_COMPOUND4args, (caddr_t)&argcompound,        \
              (xdrproc_t)xdr_COMPOUND4res,  (caddr_t)&rescompound,        \
              timeout )

#endif                          /* _FSAL_NFSV4_MACROS_H */
