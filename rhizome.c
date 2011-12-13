#include "mphlr.h"
#include "sqlite-amalgamation-3070900/sqlite3.h"
#include "sha2.h"

#define MAX_MANIFEST_VARS 256
#define MAX_MANIFEST_BYTES 8192
typedef struct rhizome_manifest {
  int manifest_bytes;
  unsigned char manifestdata[MAX_MANIFEST_BYTES];
  unsigned char manifesthash[crypto_hash_BYTES];

  /* CryptoSign key pair for this manifest.
     The filename as distributed on Rhizome will be the public key
     of this pair, thus ensuring that noone can tamper with a bundle
     except the creator. */
  unsigned char cryptoSignPublic[crypto_sign_PUBLICKEYBYTES];
  unsigned char cryptoSignSecret[crypto_sign_SECRETKEYBYTES];

  /* Set non-zero after variables have been packed and
     signature blocks appended */
  int finalised;

  int var_count;
  char *vars[MAX_MANIFEST_VARS];
  char *values[MAX_MANIFEST_VARS];

  int sig_count;
  unsigned char *signatureBlocks[MAX_MANIFEST_VARS];
  unsigned char signatureTypes[MAX_MANIFEST_VARS];
  /* 0x01 = CryptoSign signature of manifest */
  /* 0x02 = CryptoSign signature of signatory */
  int signature_errors; /* if non-zero, then manifest should not be trusted */

  
} rhizome_manifest;

long long rhizome_space=0;
char *rhizome_datastore_path=NULL;

sqlite3 *rhizome_db=NULL;

int rhizome_manifest_createid(rhizome_manifest *m);
int rhizome_write_manifest_file(rhizome_manifest *m,char *filename);
int rhizome_manifest_sign(rhizome_manifest *m);
int rhizome_drop_stored_file(char *id,int maximum_priority);
int rhizome_manifest_priority(char *id);
rhizome_manifest *rhizome_read_manifest_file(char *filename);
int rhizome_hash_file(char *filename,char *hash_out);
int rhizome_manifest_get(rhizome_manifest *m,char *var,char *value_out);
int rhizome_manifest_set_ll(rhizome_manifest *m,char *var,long long value);
int rhizome_manifest_set(rhizome_manifest *m,char *var,char *value);
long long rhizome_file_size(char *filename);
void rhizome_manifest_free(rhizome_manifest *m);
int rhizome_manifest_pack_variables(rhizome_manifest *m);
int rhizome_store_bundle(rhizome_manifest *m,char *associated_filename);
int rhizome_manifest_add_group(rhizome_manifest *m,char *groupid);

int rhizome_opendb()
{
  if (rhizome_db) return 0;
  char dbname[1024];

  if (!rhizome_datastore_path) {
    fprintf(stderr,"Cannot open rhizome database -- no path specified\n");
    exit(1);
  }
  if (strlen(rhizome_datastore_path)>1000) {
    fprintf(stderr,"Cannot open rhizome database -- data store path is too long\n");
    exit(1);
  }
  snprintf(dbname,1024,"%s/rhizome.db",rhizome_datastore_path);

  int r=sqlite3_open(dbname,&rhizome_db);
  if (r) {
    fprintf(stderr,"SQLite could not open database: %s\n",sqlite3_errmsg(rhizome_db));
    exit(1);
  }

  /* Read Rhizome configuration, and write it back out as we understand it. */
  char conf[1024];
  snprintf(conf,1024,"%s/rhizome.conf",rhizome_datastore_path);
  FILE *f=fopen(conf,"r");
  if (f) {
    char line[1024];
    line[0]=0; fgets(line,1024,f);
    while (line[0]) {
      if (sscanf(line,"space=%lld",&rhizome_space)==1) { 
	rhizome_space*=1024; /* Units are kilobytes */
      }
      line[0]=0; fgets(line,1024,f);
    }
    fclose(f);
  }
  f=fopen(conf,"w");
  if (f) {
    fprintf(f,"space=%lld\n",rhizome_space/1024LL);
    fclose(f);
  }

  /* Create tables if required */
  if (sqlite3_exec(rhizome_db,"PRAGMA auto_vacuum=2;",NULL,NULL,NULL)) {
      fprintf(stderr,"SQLite could enable incremental vacuuming: %s\n",sqlite3_errmsg(rhizome_db));
      exit(1);
  }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS GROUPS(id text not null primary key, priority integer, manifest blob, groupsecret blob);",NULL,NULL,NULL))
    {
      fprintf(stderr,"SQLite could not create GROUPS table: %s\n",sqlite3_errmsg(rhizome_db));
      exit(1);
    }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS MANIFESTS(id text not null primary key, manifest blob, version integer, privatekey blob);",NULL,NULL,NULL))
    {
      fprintf(stderr,"SQLite could not create MANIFESTS table: %s\n",sqlite3_errmsg(rhizome_db));
      exit(1);
    }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS FILES(id text not null primary key, data blob, length integer, highestpriority integer);",NULL,NULL,NULL))
    {
      fprintf(stderr,"SQLite could not create FILES table: %s\n",sqlite3_errmsg(rhizome_db));
      exit(1);
    }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS FILEMANIFESTS(fileid text not null primary key, manifestid text not null);",NULL,NULL,NULL))
    {
      fprintf(stderr,"SQLite could not create FILEMANIFESTS table: %s\n",sqlite3_errmsg(rhizome_db));
      exit(1);
    }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS MANIFESTGROUPS(manifestid text not null primary key, groupid text not null);",NULL,NULL,NULL))
    {
      fprintf(stderr,"SQLite could not create MANIFESTGROUPS table: %s\n",sqlite3_errmsg(rhizome_db));
      exit(1);
    }
  
  /* XXX Setup special groups, e.g., Serval Software and Serval Optional Data */

  return 0;
}

/* 
   Convenience wrapper for executing an SQL command that returns a single int64 value 
 */
long long sqlite_exec_int64(char *sqlformat,...)
{
  if (!rhizome_db) rhizome_opendb();

  va_list ap,ap2;
  char sqlstatement[8192];

  va_start(ap,sqlformat);
  va_copy(ap2,ap);

  vsnprintf(sqlstatement,8192,sqlformat,ap2); sqlstatement[8191]=0;

  va_end(ap);

  sqlite3_stmt *statement;
  if (sqlite3_prepare_v2(rhizome_db,sqlstatement,-1,&statement,NULL)!=SQLITE_OK)
    {
      sqlite3_close(rhizome_db);
      rhizome_db=NULL;
      return WHY("Could not prepare sql statement.");
    }
   if (sqlite3_step(statement) == SQLITE_ROW)
     {
       if (sqlite3_column_count(statement)!=1) {
	 sqlite3_finalize(statement);
	 return -1;
       }
       long long result= sqlite3_column_int(statement,0);
       sqlite3_finalize(statement);
       return result;
     }
   sqlite3_finalize(statement);
   return -1;
}

long long rhizome_database_used_bytes()
{
  long long db_page_size=sqlite_exec_int64("PRAGMA page_size;");
  long long db_page_count=sqlite_exec_int64("PRAGMA page_count;");
  long long db_free_page_count=sqlite_exec_int64("PRAGMA free_count;");
  return db_page_size*(db_page_count-db_free_page_count);
}

int rhizome_make_space(int group_priority, long long bytes)
{
  sqlite3_stmt *statement;

  /* Asked for impossibly large amount */
  if (bytes>=(rhizome_space-65536)) return -1;

  long long db_used=rhizome_database_used_bytes(); 
  
  /* If there is already enough space now, then do nothing more */
  if (db_used<=(rhizome_space-bytes-65536)) return 0;

  /* Okay, not enough space, so free up some. */
  char sql[1024];
  snprintf(sql,1024,"select id,length from files where highestpriority<%d order by descending length",group_priority);
  if(sqlite3_prepare_v2(rhizome_db,sql, -1, &statement, NULL) != SQLITE_OK )
    {
      fprintf(stderr,"SQLite error running query '%s': %s\n",sql,sqlite3_errmsg(rhizome_db));
      sqlite3_close(rhizome_db);
      rhizome_db=NULL;
      exit(-1);
    }

  while ( bytes>(rhizome_space-65536-rhizome_database_used_bytes()) && sqlite3_step(statement) == SQLITE_ROW)
    {
      /* Make sure we can drop this blob, and if so drop it, and recalculate number of bytes required */
      char *id;
      long long length;

      /* Get values */
      if (sqlite3_column_type(statement, 0)==SQLITE_TEXT) id=sqlite3_column_text(statement, 0);
      else {
	fprintf(stderr,"Incorrect type in id column of files table.\n");
	continue; }
      if (sqlite3_column_type(statement, 1)==SQLITE_INTEGER) length=sqlite3_column_int(statement, 1);
      else {
	fprintf(stderr,"Incorrect type in length column of files table.\n");
	continue; }
      
      /* Try to drop this file from storage, discarding any references that do not trump the priority of this
	 request.  The query done earlier should ensure this, but it doesn't hurt to be paranoid, and it also
	 protects against inconsistency in the database. */
      rhizome_drop_stored_file(id,group_priority+1);
    }
  sqlite3_finalize(statement);

  long long equal_priority_larger_file_space_used = sqlite_exec_int64("SELECT COUNT(length) FROM FILES WHERE highestpriority=%d and length>%lld",group_priority,bytes);
  /* XXX Get rid of any equal priority files that are larger than this one */

  /* XXX Get rid of any higher priority files that are not relevant in this time or location */

  /* Couldn't make space */
  return WHY("Not implemented");
}

/* Drop the specified file from storage, and any manifests that reference it, 
   provided that none of those manifests are being retained at a higher priority
   than the maximum specified here. */
int rhizome_drop_stored_file(char *id,int maximum_priority)
{
  char sql[1024];
  sqlite3_stmt *statement;
  int cannot_drop=0;

  if (strlen(id)>70) return -1;

  snprintf(sql,1024,"select manifests.id from manifests,filemanifests where manifests.id==filemanifests.manifestid and filemanifests.fileid='%s'",
	   id);
  if(sqlite3_prepare_v2(rhizome_db,sql, -1, &statement, NULL) != SQLITE_OK )
    {
      fprintf(stderr,"SQLite error running query '%s': %s\n",sql,sqlite3_errmsg(rhizome_db));
      sqlite3_close(rhizome_db);
      rhizome_db=NULL;
      exit(-1);
    }

  while ( sqlite3_step(statement) == SQLITE_ROW)
    {
      /* Find manifests for this file */
      char *id;
      if (sqlite3_column_type(statement, 0)==SQLITE_TEXT) id=sqlite3_column_text(statement, 0);
      else {
	fprintf(stderr,"Incorrect type in id column of manifests table.\n");
	continue; }
            
      /* Check that manifest is not part of a higher priority group.
	 If so, we cannot drop the manifest or the file.
         However, we will keep iterating, as we can still drop any other manifests pointing to this file
	 that are lower priority, and thus free up a little space. */
      if (rhizome_manifest_priority(id)>maximum_priority) {
	cannot_drop=1;
      } else {
	sqlite_exec_int64("delete from filemanifests where manifestid='%s';",id);
	sqlite_exec_int64("delete from manifests where manifestid='%s';",id);
	sqlite_exec_int64("delete from manifestgroups where manifestid='%s';",id);	
      }
    }
  sqlite3_finalize(statement);

  if (!cannot_drop) {
    sqlite_exec_int64("delete from filemanifests where fileid='%s';",id);
    sqlite_exec_int64("delete from files where id='%s';",id);
  }
  return 0;
}

/* XXX Requires a messy join that might be slow. */
int rhizome_manifest_priority(char *id)
{
  long long result = sqlite_exec_int64("select max(groups.priorty) from groups,manifests,manifestgroups where manifests.id='%s' and groups.id=manifestgroups.groupid and manifestgroups.manifestid=manifests.id;",id);
  return result;
}

/* Import a bundle from the inbox folder.
   Check that the manifest prototype is valid, and if so, complete it, and sign it if required and possible.
   The file should be included in the specified rhizome groups, if possible.
   (some groups may be closed groups that we do not have the private key for.)
*/
int rhizome_bundle_import(char *bundle,char *groups[],int verifyP, int checkFileP, int signP)
{
  char filename[1024];
  char manifestname[1024];
  char buffer[1024];
  
  snprintf(filename,1024,"%s/import/file.%s",rhizome_datastore_path,bundle); filename[1023]=0;
  snprintf(manifestname,1024,"%s/manifest.%s",rhizome_datastore_path,bundle); manifestname[1023]=0;

  /* Open files */
  rhizome_manifest *m=rhizome_read_manifest_file(manifestname);
  if (!m) return WHY("Could not read manifest file.");
  char hexhash[SHA512_DIGEST_STRING_LENGTH];

  if (checkFileP||signP) {
    if (rhizome_hash_file(filename,hexhash))
      { rhizome_manifest_free(m); return WHY("Could not hash file."); }
  }

  if (verifyP)
    {
      /* Make sure hashes match.
	 Make sure that no signature verification errors were spotted on loading. */
      int verifyErrors=0;
      char mhexhash[1024];
      if (checkFileP) {
	if (rhizome_manifest_get(m,"filehash",mhexhash)==0)
	  if (strcmp(hexhash,mhexhash)) verifyErrors++; }
      if (m->signature_errors) verifyErrors+=m->signature_errors;
      if (verifyErrors) {
	rhizome_manifest_free(m);
	unlink(manifestname);
	unlink(filename);
	return WHY("Errors encountered verifying bundle manifest");
      }
    }

  if (!verifyP) {
    if (rhizome_manifest_get(m,"id",buffer)!=0) {
      /* No bundle id (256 bit random string being a public key in the NaCl CryptoSign crypto system),
	 so create one, and keep the private key handy. */
      rhizome_manifest_createid(m);
    }
    rhizome_manifest_set(m,"filehash",hexhash);
    if (rhizome_manifest_get(m,"version",buffer)!=0)
      /* Version not set, so set one */
      rhizome_manifest_set_ll(m,"version",overlay_time_in_ms());
    rhizome_manifest_set_ll(m,"first_byte",0);
    rhizome_manifest_set_ll(m,"last_byte",rhizome_file_size(filename));
  }
   
  /* Convert to final form for signing and writing to disk */
  rhizome_manifest_pack_variables(m);
    
  /* Sign it */
  if (signP) rhizome_manifest_sign(m);

  /* Add group memberships */
  int i;
  for(i=0;groups[i];i++) rhizome_manifest_add_group(m,groups[i]);

  /* Write manifest back to disk */
  if (rhizome_write_manifest_file(m,manifestname)) {
    rhizome_manifest_free(m);
    return WHY("Could not write manifest file.");
  }

  /* Okay, it is written, and can be put directly into the rhizome database now */
  int r=rhizome_store_bundle(m,filename);
  if (!r) {
    unlink(manifestname);
    unlink(filename);
    return 0;
  }

  return -1;
}

/* Update an existing Rhizome bundle */
int rhizome_bundle_push_update(char *id,long long version,unsigned char *data,int appendP)
{
  return WHY("Not implemented");
}

rhizome_manifest *rhizome_read_manifest_file(char *filename)
{
  rhizome_manifest *m = calloc(sizeof(rhizome_manifest),1);
  if (!m) return NULL;

  FILE *f=fopen(filename,"r");
  if (!f) { rhizome_manifest_free(m); return NULL; }
  m->manifest_bytes = fread(m->manifestdata,1,MAX_MANIFEST_BYTES,f);
  fclose(f);

  /* Parse out variables, signature etc */
  int ofs=0;
  while(ofs<m->manifest_bytes&&m->manifestdata[ofs])
    {
      int i;
      char line[1024],var[1024],value[1024];
      while(ofs<m->manifest_bytes&&
	    (m->manifestdata[ofs]==0x0a||
	     m->manifestdata[ofs]==0x09||
	     m->manifestdata[ofs]==0x20||
	     m->manifestdata[ofs]==0x0d)) ofs++;
      for(i=0;i<(ofs-m->manifest_bytes)
	    &&(i<1023)
	    &&m->manifestdata[ofs]!=0x00
	    &&m->manifestdata[ofs]!=0x0d
	    &&m->manifestdata[ofs]!=0x0a;i++)
	    line[i]=m->manifestdata[ofs+i];
      line[i]=0;
      /* Ignore blank lines */
      if (line[0]==0) continue;
      if (sscanf(line,"%[^=]=%[^\n\r]",var,value)==2)
	{
	  if (rhizome_manifest_get(m,var,NULL)==0) {
	    WHY("Error in manifest file (duplicate variable -- keeping first value).");
	  }
	  if (m->var_count<MAX_MANIFEST_VARS)
	    {
	      m->vars[m->var_count]=strdup(var);
	      m->values[m->var_count]=strdup(value);
	      m->var_count++;
	    }
	}
      else
	{
	  /* Error in manifest file.
	     Silently ignore for now. */
	  WHY("Error in manifest file (badly formatted line).");
	}
    }
  /* The null byte gets included in the check sum */
  if (ofs<m->manifest_bytes) ofs++;

  /* Remember where the text ends */
  int end_of_text=ofs;

  /* Calculate hash of the text part of the file, as we need to couple this with
     each signature block to */
  unsigned char manifest_hash[crypto_hash_BYTES];
  crypto_hash(manifest_hash,m->manifestdata,end_of_text);

  /* Read signature blocks from file.
     XXX - What additional information/restrictions should the
     signatures have?  start/expiry times? geo bounding box? 
     Those elements all need to be included in the hash */
  WHY("Signature verification not implemented");

  WHY("Group membership signature reading not implemented (are we still doing it this way?)");
  
  m->manifest_bytes=end_of_text;

  WHY("Incomplete");

  rhizome_manifest_free(m);
  return NULL;
}

int rhizome_hash_file(char *filename,char *hash_out)
{
  /* Gnarf! NaCl's crypto_hash() function needs the whole file passed in in one
     go.  Trouble is, we need to run Serval DNA on filesystems that lack mmap(),
     and may be very resource constrained. Thus we need a streamable SHA-512
     implementation.
  */
  FILE *f=fopen(filename,"r");
  if (!f) return WHY("Could not open file for reading to calculage SHA512 hash.");
  unsigned char buffer[8192];
  int r;

  SHA512_CTX context;
  SHA512_Init(&context);

  while(!feof(f)) {
    r=fread(buffer,1,8192,f);
    if (r>0) SHA512_Update(&context,buffer,r);
  }

  SHA512_End(&context,(char *)hash_out);
  return 0;
}

int rhizome_manifest_get(rhizome_manifest *m,char *var,char *out)
{
  int i;

  if (!m) return -1;

  for(i=0;i<m->var_count;i++)
    if (!strcmp(m->vars[i],var)) {
      if (out) strcpy(m->values[i],out);
      return 0;
    }
  return -1;
}

int rhizome_manifest_set(rhizome_manifest *m,char *var,char *value)
{
  int i;

  if (!m) return -1;

  for(i=0;i<m->var_count;i++)
    if (!strcmp(m->vars[i],var)) {
      free(m->values[i]); 
      m->values[i]=strdup(value);
      return 0;
    }

  if (m->var_count>=MAX_MANIFEST_VARS) return -1;
  
  m->vars[m->var_count]=strdup(var);
  m->values[m->var_count]=strdup(value);
  m->var_count++;

  return 0;
}

int rhizome_manifest_set_ll(rhizome_manifest *m,char *var,long long value)
{
  char svalue[100];

  snprintf(svalue,1024,"%lld",value);

  return rhizome_manifest_set(m,var,svalue);
}

long long rhizome_file_size(char *filename)
{
  FILE *f;

  /* XXX really should just use stat instead of opening the file */
  f=fopen(filename,"r");
  fseek(f,0,SEEK_END);
  long long size=ftello(f);
  fclose(f);
  return size;
}

void rhizome_manifest_free(rhizome_manifest *m)
{
  if (!m) return;

  int i;
  for(i=0;i<m->var_count;i++)
    { free(m->vars[i]); free(m->values[i]); }

  WHY("Doesn't free signatures yet");

  free(m);

  return;
}

/* Convert variable list to string, complaining if it ends up
   too long. 
   Signatures etc will be added later. */
int rhizome_manifest_pack_variables(rhizome_manifest *m)
{
  int i,ofs=0;

  for(i=0;i<m->var_count;i++)
    {
      if ((ofs+strlen(m->vars[i])+1+strlen(m->values[i])+1+1)>MAX_MANIFEST_BYTES)
	return WHY("Manifest variables too long in total to fit in MAX_MANIFEST_BYTES");
      snprintf((char *)&m->manifestdata[ofs],MAX_MANIFEST_BYTES-ofs,"%s=%s\n",
	       m->vars[i],m->values[i]);
      ofs+=strlen((char *)&m->manifestdata[ofs]);
    }
  m->manifest_bytes=ofs;

  return 0;
}

/* Sign this manifest using our own private CryptoSign key */
int rhizome_manifest_sign(rhizome_manifest *m)
{
  return WHY("Not implemented.");
}

int rhizome_write_manifest_file(rhizome_manifest *m,char *filename)
{
  if (!m) return WHY("Manifest is null.");
  if (!m->finalised) return WHY("Manifest must be finalised before it can be written.");
  FILE *f=fopen(filename,"w");
  int r=fwrite(m->manifestdata,m->manifest_bytes,1,f);
  fclose(f);
  if (r!=1) return WHY("Failed to fwrite() manifest file.");
  return 0;
}

int rhizome_manifest_createid(rhizome_manifest *m)
{
  return crypto_sign_keypair(m->cryptoSignPublic,m->cryptoSignSecret);
}

/*
  Store the specified manifest into the sqlite database.
  We assume that sufficient space has been made for us.
  The manifest should be finalised, and so we don't need to
  look at the underlying manifest file, but can just write m->manifest_data
  as a blob.

  associated_filename needs to be read in and stored as a blob.  Hopefully that
  can be done in pieces so that we don't have memory exhaustion issues on small
  architectures.  However, we do know it's hash apriori from m, and so we can
  skip loading the file in if it is already stored.

  We need to also need to create the appropriate row(s) in the MANIFESTS, FILES, 
  FILEMANIFESTS and MANIFESTGROUPS tables.
 */
int rhizome_store_bundle(rhizome_manifest *m,char *associated_filename)
{
  return WHY("Not implemented.");
}

/*
  Adds a group that this bundle should be present in.  If we have the means to sign
  the bundle as a member of that group, then we create the appropriate signature block.
  The group signature blocks, like all signature blocks, will be appended to the
  manifest data during the finalisation process.
 */
int rhizome_manifest_add_group(rhizome_manifest *m,char *groupid)
{
  return WHY("Not implemented.");
}