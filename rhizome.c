/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "serval.h"
#include "rhizome.h"
#include <stdlib.h>

static int rhizome_enabled_flag = -1; // unknown
int rhizome_fetch_interval_ms = -1;

/* Configure rhizome.
   @author Andrew Bettison <andrew@servalproject.com>
 */
int rhizome_configure()
{
  rhizome_enabled_flag = confValueGetBoolean("rhizome.enable", 1);
  rhizome_fetch_interval_ms = (int) confValueGetInt64Range("rhizome.fetch_interval_ms", 3000, 1, 3600000);
  return 0;
}

int rhizome_enabled()
{
  if (rhizome_enabled_flag < 0)
    rhizome_configure();
  return rhizome_enabled_flag;
}

/* Import a bundle from the inbox folder.  The bundle is contained a pair of files, one containing
   the manifest and the optional other containing the payload.

   The logic is all in rhizome_add_manifest().  This function just wraps that function and manages
   file and object buffers and lifetimes.
*/

int rhizome_bundle_import(rhizome_manifest *m_in, rhizome_manifest **m_out, 
			  const char *bundle, int ttl)
{
  if (debug & DEBUG_RHIZOME)
    DEBUGF("rhizome_bundle_import(m_in=%p, m_out=%p, bundle=%s, ttl=%d)",
	  m_in, m_out, bundle ? bundle : "(null)", ttl);
  if (m_out) *m_out = NULL;

  char filename[1024];
  char manifestname[1024];

  /* make sure import path exists */
  if (create_rhizome_import_dir() == -1)
    return -1;

  if (!FORM_RHIZOME_IMPORT_PATH(filename, "file.%s", bundle)
   || !FORM_RHIZOME_IMPORT_PATH(manifestname, "manifest.%s", bundle))
    return WHY("Manifest bundle name too long");

  /* Read manifest file if no manifest was given */
  rhizome_manifest *m = m_in;
  if (!m_in) {
    m = rhizome_new_manifest();
    if (!m)
      return WHY("Out of manifests.");
    if (rhizome_read_manifest_file(m, manifestname, 0 /* file not buffer */) == -1) {
      rhizome_manifest_free(m);
      return WHY("Could not read manifest file.");
    } else if (rhizome_manifest_verify(m)) {
      rhizome_manifest_free(m);
      return WHY("Could not verify manifest file.");
    }
  }

  /* Add the manifest and its associated file to the Rhizome database. */
  m->dataFileName = strdup(filename);
  if (rhizome_manifest_check_file(m))
    return WHY("File does not belong to manifest");
  int ret=rhizome_manifest_check_duplicate(m,NULL);
  if (!ret) rhizome_add_manifest(m, ttl);
  unlink(filename);
  if (ret == -1) {
    WHY("rhizome_add_manifest() failed");
    unlink(manifestname);
  } else {
    /* >>> For testing, write manifest file back to disk and leave it there */
    // unlink(manifestname);
    if (rhizome_write_manifest_file(m, manifestname))
      ret = WHY("Could not write manifest file.");
  }

  /* If the manifest structure was allocated in this function, and it is not being returned to the
     caller, then this function is responsible for freeing it */
  if (m_out)
    *m_out = m;
  else if (!m_in)
    rhizome_manifest_free(m);

  return ret;
}

int rhizome_manifest_check_sanity(rhizome_manifest *m_in)
{
  /* Ensure manifest meets basic sanity checks. */
  const char *service = rhizome_manifest_get(m_in, "service", NULL, 0);
  const char *sender = rhizome_manifest_get(m_in, "sender", NULL, 0);
  const char *recipient = rhizome_manifest_get(m_in, "recipient", NULL, 0);
  
  if (service == NULL || !service[0])
      return WHY("Manifest missing 'service' field");
  if (rhizome_manifest_get_ll(m_in, "date") == -1)
      return WHY("Manifest missing 'date' field");
  if (strcasecmp(service, RHIZOME_SERVICE_FILE) == 0) {
    const char *name = rhizome_manifest_get(m_in, "name", NULL, 0);
    if (name == NULL)
      return WHY("Manifest missing 'name' field");
  } else if (strcasecmp(service, RHIZOME_SERVICE_MESHMS) == 0) {
    if (sender == NULL || !sender[0])
      return WHY("MeshMS Manifest missing 'sender' field");
    if (!str_is_subscriber_id(sender))
      return WHYF("MeshMS Manifest contains invalid 'sender' field: %s", sender);
    if (recipient == NULL || !recipient[0])
      return WHY("MeshMS Manifest missing 'recipient' field");
    if (!str_is_subscriber_id(recipient))
      return WHYF("MeshMS Manifest contains invalid 'recipient' field: %s", recipient);
  } else {
    return WHY("Invalid service type");
  }
  if (debug & DEBUG_RHIZOME)
    DEBUGF("sender='%s'", sender ? sender : "(null)");

  /* passes all sanity checks */
  return 0;
}


/*
  A bundle can either be an ordinary manifest-payload pair, or a group description.
  
  - Group descriptions are manifests with no payload that have the "isagroup" variable set.  They
    get stored in the manifests table AND a reference is added to the grouplist table.  Any
    manifest, including any group manifest, may be a member of zero or one group.  This allows a
    nested, i.e., multi-level group hierarchy where sub-groups will only typically be discovered
    by joining the parent group.
*/

int rhizome_manifest_bind_id(rhizome_manifest *m_in, const unsigned char *authorSid)
{
  rhizome_manifest_createid(m_in);
  /* The ID is implicit in transit, but we need to store it in the file, so that reimporting
     manifests on receiver nodes works easily.  We might implement something that strips the id
     variable out of the manifest when sending it, or some other scheme to avoid sending all the
     extra bytes. */
  char id[RHIZOME_MANIFEST_ID_STRLEN + 1];
  rhizome_bytes_to_hex_upper(m_in->cryptoSignPublic, id, RHIZOME_MANIFEST_ID_BYTES);
  rhizome_manifest_set(m_in, "id", id);
  if (authorSid) {
    /* Set the BK using the provided authorship information.
       Serval Security Framework defines BK as being:
       BK = privateKey XOR sha512(RS##BID), where BID = cryptoSignPublic, 
       and RS is the rhizome secret for the specified author. 
       The nice thing about this specification is that:
       privateKey = BK XOR sha512(RS##BID), so the same function can be used
       to encrypt and decrypt the BK field. */
    unsigned char bkbytes[RHIZOME_BUNDLE_KEY_BYTES];
    if (rhizome_bk_xor(authorSid, m_in->cryptoSignPublic, m_in->cryptoSignSecret, bkbytes) == 0) {
      char bkhex[RHIZOME_BUNDLE_KEY_STRLEN + 1];
      (void) tohex(bkhex, bkbytes, RHIZOME_BUNDLE_KEY_BYTES);
      if (debug&DEBUG_RHIZOME) DEBUGF("set BK=%s", bkhex);
      rhizome_manifest_set(m_in, "BK", bkhex);
    } else {
      return WHY("Failed to set BK");
    }
  }
  return 0;
}

int rhizome_manifest_bind_file(rhizome_manifest *m_in,const char *filename,int encryptP)
{
  /* Keep payload file name handy for later */
  m_in->dataFileName = strdup(filename);

  /* Keep note as to whether we are supposed to be encrypting this file or not */
  m_in->payloadEncryption=encryptP;
  if (encryptP) rhizome_manifest_set_ll(m_in,"crypt",1); 
  else rhizome_manifest_set_ll(m_in,"crypt",0);

  /* Get length of payload.  An empty filename means empty payload. */
  if (filename[0]) {
    struct stat stat;
    if (lstat(filename,&stat))
      return WHYF("Could not stat() payload file '%s'",filename);
    m_in->fileLength = stat.st_size;
  } else
    m_in->fileLength = 0;
  if (debug & DEBUG_RHIZOME)
    DEBUGF("filename=%s, fileLength=%lld", filename, m_in->fileLength);
  rhizome_manifest_set_ll(m_in,"filesize",m_in->fileLength);

  /* Compute hash of non-empty payload */
  if (m_in->fileLength != 0) {
    char hexhashbuf[RHIZOME_FILEHASH_STRLEN + 1];
    if (rhizome_hash_file(m_in,filename, hexhashbuf))
      return WHY("Could not hash file.");
    memcpy(&m_in->fileHexHash[0], &hexhashbuf[0], sizeof hexhashbuf);
    rhizome_manifest_set(m_in, "filehash", m_in->fileHexHash);
    m_in->fileHashedP = 1;
  } else {
    m_in->fileHexHash[0] = '\0';
    rhizome_manifest_del(m_in, "filehash");
    m_in->fileHashedP = 0;
  }
  
  return 0;
}

int rhizome_manifest_check_file(rhizome_manifest *m_in)
{
  long long gotfile = 0;
  if (sqlite_exec_int64(&gotfile, "SELECT COUNT(*) FROM FILES WHERE ID='%s' and datavalid=1;", m_in->fileHexHash) != 1) {
    WHYF("Failed to count files");
    return 0;
  }
  if (gotfile) {
    DEBUGF("Skipping file checks for bundle, as file is already in the database");
    return 0;
  }

  /* Find out whether the payload is expected to be encrypted or not */
  m_in->payloadEncryption=rhizome_manifest_get_ll(m_in, "crypt");
  
  /* Check payload file is accessible and discover its length, then check that it matches
     the file size stored in the manifest */
  long long mfilesize = rhizome_manifest_get_ll(m_in, "filesize");
  m_in->fileLength = 0;
  if (m_in->dataFileName[0]) {
    struct stat stat;
    if (lstat(m_in->dataFileName,&stat) == -1) {
      if (errno != ENOENT || mfilesize != 0)
	return WHYF_perror("stat(%s)", m_in->dataFileName);
    } else {
      m_in->fileLength = stat.st_size;
    }
  }
  if (debug & DEBUG_RHIZOME)
    DEBUGF("filename=%s, fileLength=%lld", m_in->dataFileName, m_in->fileLength);
  if (mfilesize != -1 && mfilesize != m_in->fileLength) {
    WHYF("Manifest.filesize (%lld) != actual file size (%lld)", mfilesize, m_in->fileLength);
    return -1;
  }

  /* If payload is empty, ensure manifest has not file hash, otherwis compute the hash of the
     payload and check that it matches manifest. */
  const char *mhexhash = rhizome_manifest_get(m_in, "filehash", NULL, 0);
  if (m_in->fileLength != 0) {
    char hexhashbuf[RHIZOME_FILEHASH_STRLEN + 1];
    if (rhizome_hash_file(m_in,m_in->dataFileName, hexhashbuf))
      return WHY("Could not hash file.");
    memcpy(&m_in->fileHexHash[0], &hexhashbuf[0], sizeof hexhashbuf);
    m_in->fileHashedP = 1;
    if (!mhexhash) return WHY("manifest contains no file hash");
    if (mhexhash && strcmp(m_in->fileHexHash, mhexhash)) {
      WHYF("Manifest.filehash (%s) does not match payload hash (%s)", mhexhash, m_in->fileHexHash);
      return -1;
    }
  } else {
    if (mhexhash != NULL) {
      WHYF("Manifest.filehash (%s) should be absent for empty payload", mhexhash);
      return -1;
    }
  }

  return 0;
}

/* Check if a manifest is already stored for the same payload with the same details.
   This catches the case of "dna rhizome add file <filename>" on the same file more than once.
   (Debounce!) */
int rhizome_manifest_check_duplicate(rhizome_manifest *m_in,rhizome_manifest **m_out)
{
  if (debug & DEBUG_RHIZOME) DEBUG("Checking for duplicate");
  if (m_out) *m_out = NULL; 
  rhizome_manifest *dupm = NULL;
  if (rhizome_find_duplicate(m_in, &dupm,0 /* version doesn't matter */) == -1)
    return WHY("Errors encountered searching for duplicate manifest");
  if (dupm) {
    /* If the caller wants the duplicate manifest, it must be finalised, otherwise discarded. */
    if (m_out) {
      *m_out = dupm;
    }
    else
      rhizome_manifest_free(dupm);
    if (debug & DEBUG_RHIZOME) DEBUG("Found a duplicate");
    return 2;
  }
  if (debug & DEBUG_RHIZOME) DEBUG("No duplicate found");
  return 0;
}

int rhizome_add_manifest(rhizome_manifest *m_in,int ttl)
{
  if (debug & DEBUG_RHIZOME)
    DEBUGF("rhizome_add_manifest(m_in=%p, ttl=%d)",m_in, ttl);

  if (m_in->finalised==0)
    return WHY("Manifest must be finalised before being stored");

  /* Store time to live, clamped to within legal range */
  m_in->ttl = ttl < 0 ? 0 : ttl > 254 ? 254 : ttl;

  if (rhizome_manifest_check_sanity(m_in))
    return WHY("Sanity checks on manifest failed");

  if (rhizome_manifest_check_file(m_in))
    return WHY("File does not belong to this manifest");

  /* Get manifest version number. */
  m_in->version = rhizome_manifest_get_ll(m_in, "version");
  if (m_in->version==-1) 
    return WHY("Manifest must have a version number");

  /* Supply manifest version number if missing, so we can do the version check below */
  if (m_in->version == -1) {
    m_in->version = gettime_ms();
    rhizome_manifest_set_ll(m_in, "version", m_in->version);
  }

  /* If the manifest already has an ID */
  char id[SID_STRLEN + 1];
  if (rhizome_manifest_get(m_in, "id", id, SID_STRLEN + 1)) {
    str_toupper_inplace(id);
    /* Discard the new manifest unless it is newer than the most recent known version with the same ID */
    long long storedversion = -1;
    switch (sqlite_exec_int64(&storedversion, "SELECT version from manifests where id='%s';", id)) {
      case -1:
	return WHY("Select failed");
      case 0:
	if (debug & DEBUG_RHIZOME) DEBUG("No existing manifest");
	break;
      case 1:
	if (debug & DEBUG_RHIZOME) DEBUGF("Found existing version=%lld, new version=%lld", storedversion, m_in->version);
	if (m_in->version < storedversion)
	  return WHY("Newer version exists");
	if (m_in->version == storedversion)
	  return WHY("Same version of manifest exists, not adding");
	break;
      default:
	return WHY("Select found too many rows!");
    }
  } else {
    /* no manifest ID */
    return WHY("Manifest does not have an ID");   
  }

  /* Okay, it is written, and can be put directly into the rhizome database now */
  if (rhizome_store_bundle(m_in) == -1)
    return WHY("rhizome_store_bundle() failed.");

  // This message used in tests; do not modify or remove.
  const char *service = rhizome_manifest_get(m_in, "service", NULL, 0);
  INFOF("RHIZOME ADD MANIFEST service=%s bid=%s version=%lld",
      service ? service : "NULL",
      alloca_tohex_sid(m_in->cryptoSignPublic),
      m_in->version
      );
  monitor_announce_bundle(m_in);
  return 0;
}

/* Update an existing Rhizome bundle */
int rhizome_bundle_push_update(char *id,long long version,unsigned char *data,int appendP)
{
  return WHY("Not implemented");
}

/* When voice traffic is being carried, we need to throttle Rhizome down
   to a more sensible level.  Or possibly even supress it entirely.
 */
time_ms_t rhizome_voice_timeout = -1;
int rhizome_saw_voice_traffic()
{
  /* We are in "voice mode" for a second after sending a voice frame */
  rhizome_voice_timeout=gettime_ms()+1000;
  return 0;
}
