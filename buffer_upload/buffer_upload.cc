/*   buffer_upload.c - plugin for buffering POST data on proxy server
 *   before connecting to origin server. It supports two types of buffering:
 *   memory-only buffering and disk buffering
 * 
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
//#define bool int
#include <ts/ts.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <dirent.h>
#include <unistd.h>

#define true 1
#define false 0

/* #define DEBUG 1 */
#define DEBUG_TAG "buffer_upload-dbg"

/**************************************************
   Log macros for error code return verification 
**************************************************/
#define PLUGIN_NAME "buffer_upload"
#define LOG_SET_FUNCTION_NAME(NAME) const char * FUNCTION_NAME = NAME
#define LOG_ERROR(API_NAME) {						\
    TS_ERROR("%s: %s %s %s File %s, line number %d", PLUGIN_NAME, API_NAME, "APIFAIL", \
             FUNCTION_NAME, __FILE__, __LINE__);			\
  }
#define LOG_ERROR_AND_RETURN(API_NAME) {	\
    LOG_ERROR(API_NAME);			\
    return TS_ERROR;				\
  }

#define VALID_PTR(X) ((X != NULL) && (X != TS_ERROR_PTR))
#define NOT_VALID_PTR(X) ((X == NULL) || (X == TS_ERROR_PTR))


struct upload_config_t
{
  bool use_disk_buffer;
  bool convert_url;
  size_t mem_buffer_size;
  size_t chunk_size;
  char *url_list_file;
  size_t max_url_length;
  int url_num;
  char **urls;
  char *base_dir;
  int subdir_num;
  int thread_num;
};

typedef struct upload_config_t upload_config;

enum config_type
{
  TYPE_INT,
  TYPE_UINT,
  TYPE_LONG,
  TYPE_ULONG,
  TYPE_STRING,
  TYPE_BOOL,
};

struct config_val_ul
{
  const char *str;
  enum config_type type;
  void *val;
};

static TSStat upload_vc_count;

static upload_config *uconfig = NULL;

struct pvc_state_t
{
  TSVConn p_vc;
  TSVIO p_read_vio;
  TSVIO p_write_vio;

  TSVConn net_vc;
  TSVIO n_read_vio;
  TSVIO n_write_vio;

  TSIOBuffer req_buffer;
  TSIOBufferReader req_reader;

  TSIOBuffer resp_buffer;
  TSIOBufferReader resp_reader;

  TSIOBufferReader req_hdr_reader;
  TSIOBuffer req_hdr_buffer;

  TSMutex disk_io_mutex;

  int fd;
  char *filename;

  int req_finished;
  int resp_finished;
  int nbytes_to_consume;
  int req_size;
  int size_written;
  int size_read;

  int write_offset;
  int read_offset;

  char *chunk_buffer;           // buffer to store the data read from disk
  int is_reading_from_disk;

  TSHttpTxn http_txnp;
};

typedef struct pvc_state_t pvc_state;

// print IOBuffer for test purpose
static void
print_buffer(TSIOBufferReader reader)
{
  TSIOBufferBlock block;
  int size;
  const char *ptr;

  block = TSIOBufferReaderStart(reader);
  while (block != NULL && block != TS_ERROR_PTR) {
    ptr = TSIOBufferBlockReadStart(block, reader, &size);
    TSDebug(DEBUG_TAG, "buffer size: %d", size);
    TSDebug(DEBUG_TAG, "buffer: %.*s", size, ptr);
    block = TSIOBufferBlockNext(block);
  }
}

static int
write_buffer_to_disk(TSIOBufferReader reader, pvc_state * my_state, TSCont contp)
{
  TSIOBufferBlock block;
  int size;
  const char *ptr;
  char *pBuf;

  LOG_SET_FUNCTION_NAME("write_buffer_to_disk");
  block = TSIOBufferReaderStart(reader);
  while (block != NULL && block != TS_ERROR_PTR) {
    ptr = TSIOBufferBlockReadStart(block, reader, &size);
    pBuf = (char *) TSmalloc(sizeof(char) * size);
    if (pBuf == NULL) {
      LOG_ERROR_AND_RETURN("TSAIOWrite");
    }
    memcpy(pBuf, ptr, size);
    if (TSAIOWrite(my_state->fd, my_state->write_offset, pBuf, size, contp) < 0) {
      LOG_ERROR_AND_RETURN("TSAIOWrite");
    }
    my_state->write_offset += size;
    block = TSIOBufferBlockNext(block);
  }
  return TS_SUCCESS;
}

static int
call_httpconnect(TSCont contp, pvc_state * my_state)
{
  LOG_SET_FUNCTION_NAME("call_httpconnect");

  unsigned int client_ip = TSHttpTxnClientIPGet(my_state->http_txnp);

  TSDebug(DEBUG_TAG, "call TSHttpConnect() ...");
  if (TSHttpConnect(htonl(client_ip), 9999, &(my_state->net_vc)) == TS_ERROR) {
    LOG_ERROR_AND_RETURN("TSHttpConnect");
  }
  my_state->p_write_vio = TSVConnWrite(my_state->p_vc, contp, my_state->resp_reader, INT_MAX);
  if (my_state->p_write_vio == TS_ERROR_PTR) {
    LOG_ERROR_AND_RETURN("TSVConnWrite");
  }
  my_state->n_read_vio = TSVConnRead(my_state->net_vc, contp, my_state->resp_buffer, INT_MAX);
  if (my_state->n_read_vio == TS_ERROR_PTR) {
    LOG_ERROR_AND_RETURN("TSVConnRead");
  }
  my_state->n_write_vio = TSVConnWrite(my_state->net_vc, contp, my_state->req_reader, INT_MAX);
  if (my_state->n_write_vio == TS_ERROR_PTR) {
    LOG_ERROR_AND_RETURN("TSVConnWrite");
  }
  return TS_SUCCESS;
}

static void
pvc_cleanup(TSCont contp, pvc_state * my_state)
{
  LOG_SET_FUNCTION_NAME("pvc_cleanup");

  if (my_state->req_buffer) {
    if (TSIOBufferReaderFree(my_state->req_reader) == TS_ERROR) {
      LOG_ERROR("TSIOBufferReaderFree");
    }
    my_state->req_reader = NULL;
    if (TSIOBufferDestroy(my_state->req_buffer) == TS_ERROR) {
      LOG_ERROR("TSIOBufferDestroy");
    }
    my_state->req_buffer = NULL;
  }

  if (my_state->resp_buffer) {
    if (TSIOBufferReaderFree(my_state->resp_reader) == TS_ERROR) {
      LOG_ERROR("TSIOBufferReaderFree");
    }
    my_state->resp_reader = NULL;
    if (TSIOBufferDestroy(my_state->resp_buffer) == TS_ERROR) {
      LOG_ERROR("TSIOBufferDestroy");
    }
    my_state->resp_buffer = NULL;
  }

  if (my_state->req_hdr_buffer) {
    if (TSIOBufferReaderFree(my_state->req_hdr_reader) == TS_ERROR) {
      LOG_ERROR("TSIOBufferReaderFree");
    }
    my_state->req_hdr_reader = NULL;
    if (TSIOBufferDestroy(my_state->req_hdr_buffer) == TS_ERROR) {
      LOG_ERROR("TSIOBufferDestroy");
    }
    my_state->req_hdr_buffer = NULL;
  }

  if (uconfig->use_disk_buffer && my_state->fd != -1) {
    close(my_state->fd);
    remove(my_state->filename);
    my_state->fd = -1;
  }

  if (my_state->filename) {
    free(my_state->filename);
    my_state->filename = NULL;
  }

  if (my_state->chunk_buffer) {
    TSfree(my_state->chunk_buffer);
    my_state->chunk_buffer = NULL;
  }

  TSfree(my_state);
  if (TSContDestroy(contp) == TS_ERROR) {
    LOG_ERROR("TSContDestroy");
  }

  /* Decrement upload_vc_count */
  TSStatDecrement(upload_vc_count);
}

static void
pvc_check_done(TSCont contp, pvc_state * my_state)
{
  LOG_SET_FUNCTION_NAME("pvc_check_done");

  if (my_state->req_finished && my_state->resp_finished) {
    if (TSVConnClose(my_state->p_vc) == TS_ERROR) {
      LOG_ERROR("TSVConnClose");
    }
    if (TSVConnClose(my_state->net_vc) == TS_ERROR) {
      LOG_ERROR("TSVConnClose");
    }
    pvc_cleanup(contp, my_state);
  }
}

static void
pvc_process_accept(TSCont contp, int event, void *edata, pvc_state * my_state)
{
  LOG_SET_FUNCTION_NAME("pvc_process_accept");

  //TSDebug(DEBUG_TAG, "plugin called: pvc_process_accept with event %d", event);

  if (event == TS_EVENT_NET_ACCEPT) {
    my_state->p_vc = (TSVConn) edata;

    my_state->req_buffer = TSIOBufferCreate();
    my_state->req_reader = TSIOBufferReaderAlloc(my_state->req_buffer);
    // set the maximum memory buffer size for request (both request header and post data), default is 32K
    // only apply to memory buffer mode
    if (!uconfig->use_disk_buffer && TSIOBufferWaterMarkSet(my_state->req_buffer, uconfig->mem_buffer_size) == TS_ERROR) {
      LOG_ERROR("TSIOBufferWaterMarkSet");
    }
    my_state->resp_buffer = TSIOBufferCreate();
    my_state->resp_reader = TSIOBufferReaderAlloc(my_state->resp_buffer);

    if ((my_state->req_buffer == TS_ERROR_PTR) || (my_state->req_reader == TS_ERROR_PTR)
        || (my_state->resp_buffer == TS_ERROR_PTR) || (my_state->resp_reader == TS_ERROR_PTR)) {
      LOG_ERROR("TSIOBufferCreate || TSIOBufferReaderAlloc");
      if (TSVConnClose(my_state->p_vc) == TS_ERROR) {
        LOG_ERROR("TSVConnClose");
      }
      pvc_cleanup(contp, my_state);
    } else {
      my_state->p_read_vio = TSVConnRead(my_state->p_vc, contp, my_state->req_buffer, INT_MAX);
      if (my_state->p_read_vio == TS_ERROR_PTR) {
        LOG_ERROR("TSVConnRead");
      }
    }
  } else if (event == TS_EVENT_NET_ACCEPT_FAILED) {
    pvc_cleanup(contp, my_state);
  } else {
    TSReleaseAssert(!"Unexpected Event");
  }
}

static void
pvc_process_p_read(TSCont contp, TSEvent event, pvc_state * my_state)
{
  LOG_SET_FUNCTION_NAME("pvc_process_p_read");
  int bytes;
  int size, consume_size;

  //TSDebug(DEBUG_TAG, "plugin called: pvc_process_p_read with event %d", event);

  switch (event) {
  case TS_EVENT_VCONN_READ_READY:
    // Here we need to replace the server request header with client request header
    // print_buffer(my_state->req_reader);
    if (my_state->nbytes_to_consume == -1) {    // -1 is the initial value
      TSHttpTxnServerReqHdrBytesGet(my_state->http_txnp, &(my_state->nbytes_to_consume));
    }
    size = TSIOBufferReaderAvail(my_state->req_reader);
    if (my_state->nbytes_to_consume > 0) {
      consume_size = (my_state->nbytes_to_consume < size) ? my_state->nbytes_to_consume : size;
      TSIOBufferReaderConsume(my_state->req_reader, consume_size);
      my_state->nbytes_to_consume -= consume_size;
      size -= consume_size;
    }
    if (my_state->nbytes_to_consume == 0) {     // the entire server request header has been consumed
      if (uconfig->use_disk_buffer) {
        TSMutexLock(my_state->disk_io_mutex);
        if (write_buffer_to_disk(my_state->req_hdr_reader, my_state, contp) == TS_ERROR) {
          LOG_ERROR("write_buffer_to_disk");
          uconfig->use_disk_buffer = 0;
          close(my_state->fd);
          remove(my_state->filename);
          my_state->fd = -1;
        }
        TSMutexUnlock(my_state->disk_io_mutex);
      }
      if (size > 0) {
        if (uconfig->use_disk_buffer) {
          TSMutexLock(my_state->disk_io_mutex);
          if (write_buffer_to_disk(my_state->req_reader, my_state, contp) == TS_ERROR) {
            TSDebug(DEBUG_TAG, "Error in writing to disk");
          }
          TSMutexUnlock(my_state->disk_io_mutex);
        } else {
          // never get chance to test this line, didn't get a test case to fall into this situation
          TSIOBufferCopy(my_state->req_hdr_reader, my_state->req_reader, size, 0);
        }
        TSIOBufferReaderConsume(my_state->req_reader, size);
      }
      if (!uconfig->use_disk_buffer) {
        size = TSIOBufferReaderAvail(my_state->req_hdr_reader);
        TSIOBufferCopy(my_state->req_buffer, my_state->req_hdr_reader, size, 0);
      }
      my_state->nbytes_to_consume = -2; // -2 indicates the header replacement is done
    }
    if (my_state->nbytes_to_consume == -2) {
      size = TSIOBufferReaderAvail(my_state->req_reader);
      if (uconfig->use_disk_buffer) {
        if (size > 0) {
          TSMutexLock(my_state->disk_io_mutex);
          if (write_buffer_to_disk(my_state->req_reader, my_state, contp) == TS_ERROR) {
            TSDebug(DEBUG_TAG, "Error in writing to disk");
          }
          TSIOBufferReaderConsume(my_state->req_reader, size);
          TSMutexUnlock(my_state->disk_io_mutex);
        }
      } else {
        // if the entire post data had been read in memory, then connect to origin server.
        if (size >= my_state->req_size) {
          if (call_httpconnect(contp, my_state) == TS_ERROR) {
            LOG_ERROR("call_httpconnect");
          }
        }
      }
    }

    break;
  case TS_EVENT_VCONN_READ_COMPLETE:
  case TS_EVENT_VCONN_EOS:
  case TS_EVENT_ERROR:
    {

      /* We're finished reading from the plugin vc */
      int ndone;

      ndone = TSVIONDoneGet(my_state->p_read_vio);
      if (ndone == TS_ERROR) {
        LOG_ERROR("TSVIODoneGet");
      }

      my_state->p_read_vio = NULL;

      if (TSVConnShutdown(my_state->p_vc, 1, 0) == TS_ERROR) {
        LOG_ERROR("TSVConnShutdown");
      }
      // if client aborted the uploading in middle, need to cleanup the file from disk
      if (event == TS_EVENT_VCONN_EOS && uconfig->use_disk_buffer && my_state->fd != -1) {
        close(my_state->fd);
        remove(my_state->filename);
        my_state->fd = -1;
      }

      break;
    }
  default:
    TSReleaseAssert(!"Unexpected Event");
    break;
  }
}

static void
pvc_process_n_write(TSCont contp, TSEvent event, pvc_state * my_state)
{
  LOG_SET_FUNCTION_NAME("pvc_process_n_write");
  int bytes;
  int size;

  //TSDebug(DEBUG_TAG, "plugin called: pvc_process_n_write with event %d", event);

  switch (event) {
  case TS_EVENT_VCONN_WRITE_READY:
    // print_buffer(my_state->req_reader);
    if (uconfig->use_disk_buffer) {
      TSMutexLock(my_state->disk_io_mutex);
      size =
        (my_state->req_size - my_state->read_offset) >
        uconfig->chunk_size ? uconfig->chunk_size : (my_state->req_size - my_state->read_offset);
      if (size > 0 && !my_state->is_reading_from_disk) {
        my_state->is_reading_from_disk = 1;
        TSAIORead(my_state->fd, my_state->read_offset, my_state->chunk_buffer, size, contp);
        my_state->read_offset += size;
      }
      TSMutexUnlock(my_state->disk_io_mutex);
    }
    break;
  case TS_EVENT_ERROR:
    if (my_state->p_read_vio) {
      if (TSVConnShutdown(my_state->p_vc, 1, 0) == TS_ERROR) {
        LOG_ERROR("TSVConnShutdown");
      }
      my_state->p_read_vio = NULL;
    }
    /* FALL THROUGH */
  case TS_EVENT_VCONN_WRITE_COMPLETE:
    /* We should have already shutdown read pvc side */
    TSAssert(my_state->p_read_vio == NULL);
    if (TSVConnShutdown(my_state->net_vc, 0, 1) == TS_ERROR) {
      LOG_ERROR("TSVConnShutdown");
    }
    my_state->req_finished = 1;

    if (uconfig->use_disk_buffer && my_state->fd != -1) {
      close(my_state->fd);
      remove(my_state->filename);
      my_state->fd = -1;
    }
    pvc_check_done(contp, my_state);
    break;

  default:
    TSReleaseAssert(!"Unexpected Event");
    break;
  }
}

static void
pvc_process_n_read(TSCont contp, TSEvent event, pvc_state * my_state)
{
  LOG_SET_FUNCTION_NAME("pvc_process_n_read");
  int bytes;

  //TSDebug(DEBUG_TAG, "plugin called: pvc_process_n_read with event %d", event);

  switch (event) {
  case TS_EVENT_VCONN_READ_READY:
    // print_buffer(my_state->resp_reader);
    if (TSVIOReenable(my_state->p_write_vio) == TS_ERROR) {
      LOG_ERROR("TSVIOReenable");
    }
    break;
  case TS_EVENT_VCONN_READ_COMPLETE:
  case TS_EVENT_VCONN_EOS:
  case TS_EVENT_ERROR:
    {
      /* We're finished reading from the plugin vc */
      int ndone;
      int todo;

      ndone = TSVIONDoneGet(my_state->n_read_vio);
      if (ndone == TS_ERROR) {
        LOG_ERROR("TSVIODoneGet");
      }

      my_state->n_read_vio = NULL;
      if (TSVIONBytesSet(my_state->p_write_vio, ndone) == TS_ERROR) {
        LOG_ERROR("TSVIONBytesSet");
      }
      if (TSVConnShutdown(my_state->net_vc, 1, 0) == TS_ERROR) {
        LOG_ERROR("TSVConnShutdown");
      }

      todo = TSVIONTodoGet(my_state->p_write_vio);
      if (todo == TS_ERROR) {
        LOG_ERROR("TSVIOTodoGet");
        /* Error so set it to 0 to cleanup */
        todo = 0;
      }

      if (todo == 0) {
        my_state->resp_finished = 1;
        if (TSVConnShutdown(my_state->p_vc, 0, 1) == TS_ERROR) {
          LOG_ERROR("TSVConnShutdown");
        }
        pvc_check_done(contp, my_state);
      } else {
        if (TSVIOReenable(my_state->p_write_vio) == TS_ERROR) {
          LOG_ERROR("TSVIOReenable");
        }
      }

      break;
    }
  default:
    TSReleaseAssert(!"Unexpected Event");
    break;
  }
}

static void
pvc_process_p_write(TSCont contp, TSEvent event, pvc_state * my_state)
{
  LOG_SET_FUNCTION_NAME("pvc_process_p_write");
  int bytes;

  //TSDebug(DEBUG_TAG, "plugin called: pvc_process_p_write with event %d", event);

  switch (event) {
  case TS_EVENT_VCONN_WRITE_READY:
    if (my_state->n_read_vio) {
      if (TSVIOReenable(my_state->n_read_vio) == TS_ERROR) {
        LOG_ERROR("TSVIOReenable");
      }
    }
    break;
  case TS_EVENT_ERROR:
    if (my_state->n_read_vio) {
      if (TSVConnShutdown(my_state->net_vc, 1, 0) == TS_ERROR) {
        LOG_ERROR("INVConnShutdown");
      }
      my_state->n_read_vio = NULL;
    }
    /* FALL THROUGH */
  case TS_EVENT_VCONN_WRITE_COMPLETE:
    /* We should have already shutdown read net side */
    TSAssert(my_state->n_read_vio == NULL);
    if (TSVConnShutdown(my_state->p_vc, 0, 1) == TS_ERROR) {
      LOG_ERROR("TSVConnShutdown");
    }
    my_state->resp_finished = 1;
    pvc_check_done(contp, my_state);
    break;
  default:
    TSReleaseAssert(!"Unexpected Event");
    break;
  }
}

static int
pvc_plugin(TSCont contp, TSEvent event, void *edata)
{
  pvc_state *my_state = TSContDataGet(contp);

  if (my_state == NULL) {
    TSReleaseAssert(!"Unexpected: my_state is NULL");
    return 0;
  }

  if (event == TS_EVENT_NET_ACCEPT || event == TS_EVENT_NET_ACCEPT_FAILED) {
    pvc_process_accept(contp, event, edata, my_state);
  } else if (edata == my_state->p_read_vio) {
    pvc_process_p_read(contp, event, my_state);
  } else if (edata == my_state->p_write_vio) {
    pvc_process_p_write(contp, event, my_state);
  } else if (edata == my_state->n_read_vio) {
    pvc_process_n_read(contp, event, my_state);
  } else if (edata == my_state->n_write_vio) {
    pvc_process_n_write(contp, event, my_state);
  } else if (event == TS_AIO_EVENT_DONE && uconfig->use_disk_buffer) {
    TSMutexLock(my_state->disk_io_mutex);
    int size = TSAIONBytesGet(edata);
    char *buf = TSAIOBufGet(edata);
    if (buf != my_state->chunk_buffer) {
      // this TS_AIO_EVENT_DONE event is from TSAIOWrite()
      TSDebug(DEBUG_TAG, "aio write size: %d", size);
      my_state->size_written += size;
      if (buf != NULL) {
        TSfree(buf);
      }
      if (my_state->size_written >= my_state->req_size) {
        // the entire post data had been written to disk  already, make the connection now
        if (call_httpconnect(contp, my_state) == TS_ERROR) {
          TSDebug(DEBUG_TAG, "call_httpconnect");
        }
      }
    } else {
      // this TS_AIO_EVENT_DONE event is from TSAIORead()
      TSDebug(DEBUG_TAG, "aio read size: %d", size);
      TSIOBufferWrite(my_state->req_buffer, my_state->chunk_buffer, size);
      my_state->size_read += size;
      if (my_state->size_read >= my_state->req_size && my_state->fd != -1) {
        close(my_state->fd);
        remove(my_state->filename);
        my_state->fd = -1;
      }
      my_state->is_reading_from_disk = 0;
      if (TSVIOReenable(my_state->n_write_vio) == TS_ERROR) {
        TS_ERROR("TSVIOReenable");
      }
    }
    TSMutexUnlock(my_state->disk_io_mutex);

  } else {
    TSDebug(DEBUG_TAG, "event: %d", event);
    TSReleaseAssert(!"Unexpected Event");
  }

  return 0;
}

/*
 *  Convert specific URL format
 */
static void
convert_url_func(TSMBuffer req_bufp, TSMLoc req_loc)
{
  TSMLoc url_loc;
  TSMLoc field_loc;
  const char *str;
  int len, port;

  url_loc = TSHttpHdrUrlGet(req_bufp, req_loc);
  if (NOT_VALID_PTR(url_loc))
    return;

  char *hostname = (char *) getenv("HOSTNAME");

  // in reverse proxy mode, TSUrlHostGet returns NULL here
  str = TSUrlHostGet(req_bufp, url_loc, &len);

  port = TSUrlPortGet(req_bufp, url_loc);

  // for now we assume the <upload proxy service domain> in the format is the hostname
  // but this needs to be verified later
  if (NOT_VALID_PTR(str) || !strncmp(str, hostname, len) && strlen(hostname) == len) {
    char *slash;
    char *colon;
    if (VALID_PTR(str))
      TSHandleStringRelease(req_bufp, url_loc, str);
    str = TSUrlPathGet(req_bufp, url_loc, &len);
    slash = strstr(str, "/");
    if (slash == NULL) {
      if (VALID_PTR(str))
        TSHandleStringRelease(req_bufp, url_loc, str);
      TSHandleMLocRelease(req_bufp, req_loc, url_loc);
      return;
    }
    char pathTmp[len + 1];
    memset(pathTmp, 0, sizeof pathTmp);
    memcpy(pathTmp, str, len);
    TSDebug(DEBUG_TAG, "convert_url_func working on path: %s", pathTmp);
    colon = strstr(str, ":");
    if (colon != NULL && colon < slash) {
      char *port_str = (char *) TSmalloc(sizeof(char) * (slash - colon));
      strncpy(port_str, colon + 1, slash - colon - 1);
      port_str[slash - colon - 1] = '\0';
      TSUrlPortSet(req_bufp, url_loc, atoi(port_str));
      TSfree(port_str);
    } else {
      if (port != 80) {
        TSUrlPortSet(req_bufp, url_loc, 80);
      }
      colon = slash;
    }

    TSUrlHostSet(req_bufp, url_loc, str, colon - str);
    TSUrlPathSet(req_bufp, url_loc, slash + 1, len - (slash - str) - 1);
    if ((field_loc = TSMimeHdrFieldRetrieve(req_bufp, req_loc, TS_MIME_FIELD_HOST)) != TS_ERROR_PTR &&
        field_loc != NULL) {
      TSMimeHdrFieldValueStringSet(req_bufp, req_loc, field_loc, 0, str, slash - str);
      TSHandleMLocRelease(req_bufp, req_loc, field_loc);
    }
  } else {
    if (VALID_PTR(str))
      TSHandleStringRelease(req_bufp, url_loc, str);
  }

  TSHandleMLocRelease(req_bufp, req_loc, url_loc);
}

static int
attach_pvc_plugin(TSCont contp, TSEvent event, void *edata)
{
  LOG_SET_FUNCTION_NAME("attach_pvc_plugin");

  TSHttpTxn txnp = (TSHttpTxn) edata;
  TSMutex mutex;
  TSCont new_cont;
  pvc_state *my_state;
  TSMBuffer req_bufp;
  TSMLoc req_loc;
  TSMLoc field_loc;
  TSMLoc url_loc;
  char *url;
  int url_len;
  int value;
  int val_len;
  int content_length = 0;
  const char *method;
  int method_len;
  const char *str;
  int str_len;
  const char *ptr;

  switch (event) {
  case TS_EVENT_HTTP_READ_REQUEST_PRE_REMAP:

    // if the request is issued by the TSHttpConnect() in this plugin, don't get in the endless cycle.
    if (TSHttpIsInternalRequest(txnp)) {
      break;
    }

    if (!TSHttpTxnClientReqGet(txnp, &req_bufp, &req_loc)) {
      LOG_ERROR("Error while retrieving client request header");
      break;
    }

    method = TSHttpHdrMethodGet(req_bufp, req_loc, &method_len);

    if (NOT_VALID_PTR(method) || method_len == 0) {
      TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
      break;
    }
    // only deal with POST method
    if (method_len != strlen(TS_HTTP_METHOD_POST) || strncasecmp(method, TS_HTTP_METHOD_POST, method_len) != 0) {
      TSHandleStringRelease(req_bufp, req_loc, method);
      TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
      break;
    }

    TSHandleStringRelease(req_bufp, req_loc, method);

    TSDebug(DEBUG_TAG, "Got POST req");
    if (uconfig->url_list_file != NULL) {
      TSDebug(DEBUG_TAG, "url_list_file != NULL");
      // check against URL list
      url_loc = TSHttpHdrUrlGet(req_bufp, req_loc);
      str = TSUrlHostGet(req_bufp, url_loc, &str_len);
      if (NOT_VALID_PTR(str) || str_len <= 0) {
        // reverse proxy mode
        field_loc = TSMimeHdrFieldFind(req_bufp, req_loc, TS_MIME_FIELD_HOST, -1);
        if (NOT_VALID_PTR(field_loc)) {
          if (VALID_PTR(str))
            TSHandleStringRelease(req_bufp, url_loc, str);
          LOG_ERROR("Host field not found.");
          TSHandleMLocRelease(req_bufp, req_loc, url_loc);
          TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
          break;
        }
        str = TSMimeHdrFieldValueGet(req_bufp, req_loc, field_loc, 0, &str_len);
        if (NOT_VALID_PTR(str) || str_len <= 0) {
          if (VALID_PTR(str))
            TSHandleStringRelease(req_bufp, field_loc, str);
          TSHandleMLocRelease(req_bufp, req_loc, field_loc);
          TSHandleMLocRelease(req_bufp, req_loc, url_loc);
          TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
          break;
        }

        char replacement_host_str[str_len + 1];
        memset(replacement_host_str, 0, sizeof replacement_host_str);
        memcpy(replacement_host_str, str, str_len);
        TSDebug(DEBUG_TAG, "Adding host to request url: %s", replacement_host_str);

        TSUrlHostSet(req_bufp, url_loc, str, str_len);

        TSHandleStringRelease(req_bufp, field_loc, str);
        TSHandleMLocRelease(req_bufp, req_loc, field_loc);
      } else {
        TSHandleStringRelease(req_bufp, url_loc, str);
      }

      int i = uconfig->url_num;
      url = TSUrlStringGet(req_bufp, url_loc, &url_len);
      if (VALID_PTR(url)) {
        char urlStr[url_len + 1];
        memset(urlStr, 0, sizeof urlStr);
        memcpy(urlStr, url, url_len);
        TSDebug(DEBUG_TAG, "Request url: %s", urlStr);

        for (i = 0; i < uconfig->url_num; i++) {
          TSDebug(DEBUG_TAG, "uconfig url: %s", uconfig->urls[i]);
          if (strncmp(url, uconfig->urls[i], url_len) == 0) {
            break;
          }
        }

        TSHandleStringRelease(req_bufp, url_loc, url);
      }
      TSHandleMLocRelease(req_bufp, req_loc, url_loc);

      if (uconfig->url_num > 0 && i == uconfig->url_num) {
        TSDebug(DEBUG_TAG, "breaking: url_num > 0 and i== url_num, URL match not found");
        TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
        break;
      }
    }

    if (uconfig->convert_url) {
      TSDebug(DEBUG_TAG, "doing convert url");
      convert_url_func(req_bufp, req_loc);
    }

    if ((field_loc = TSMimeHdrFieldRetrieve(req_bufp, req_loc, TS_MIME_FIELD_CONTENT_LENGTH)) == TS_ERROR_PTR ||
        field_loc == NULL) {
      TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
      LOG_ERROR("TSMimeHdrFieldRetrieve");
      break;
    }

    if (TSMimeHdrFieldValueIntGet(req_bufp, req_loc, field_loc, 0, &value) == TS_ERROR) {
      TSHandleMLocRelease(req_bufp, req_loc, field_loc);
      TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
      LOG_ERROR("TSMimeFieldValueGet");
    } else
      content_length = value;

    mutex = TSMutexCreate();
    if (NOT_VALID_PTR(mutex)) {
      TSHandleMLocRelease(req_bufp, req_loc, field_loc);
      TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
      LOG_ERROR("TSMutexCreate");
      break;
    }

    new_cont = TSContCreate(pvc_plugin, mutex);
    if (NOT_VALID_PTR(new_cont)) {
      TSHandleMLocRelease(req_bufp, req_loc, field_loc);
      TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
      LOG_ERROR("TSContCreate");
      break;
    }

    my_state = (pvc_state *) TSmalloc(sizeof(pvc_state));
    my_state->req_size = content_length;
    my_state->p_vc = NULL;
    my_state->p_read_vio = NULL;
    my_state->p_write_vio = NULL;

    my_state->net_vc = NULL;
    my_state->n_read_vio = NULL;
    my_state->n_write_vio = NULL;

    my_state->req_buffer = NULL;
    my_state->req_reader = NULL;
    my_state->resp_buffer = NULL;
    my_state->resp_reader = NULL;
    my_state->filename = NULL;
    my_state->fd = -1;
    my_state->disk_io_mutex = NULL;

    my_state->http_txnp = txnp; // not in use now, may need in the future

    my_state->req_finished = 0;
    my_state->resp_finished = 0;
    my_state->nbytes_to_consume = -1;   // the length of server request header to remove from incoming stream (will replace with client request header)

    my_state->size_written = 0;
    my_state->size_read = 0;
    my_state->write_offset = 0;
    my_state->read_offset = 0;
    my_state->is_reading_from_disk = 0;

    my_state->chunk_buffer = (char *) TSmalloc(sizeof(char) * uconfig->chunk_size);

    my_state->disk_io_mutex = TSMutexCreate();
    if (NOT_VALID_PTR(my_state->disk_io_mutex)) {
      LOG_ERROR("TSMutexCreate");
    }

    int size;

    my_state->req_hdr_buffer = TSIOBufferCreate();
    my_state->req_hdr_reader = TSIOBufferReaderAlloc(my_state->req_hdr_buffer);
    TSHttpHdrPrint(req_bufp, req_loc, my_state->req_hdr_buffer);
    // print_buffer(my_state->req_hdr_reader);

    my_state->req_size += TSIOBufferReaderAvail(my_state->req_hdr_reader);

    /* Increment upload_vc_count */
    TSStatIncrement(upload_vc_count);

    if (!uconfig->use_disk_buffer && my_state->req_size > uconfig->mem_buffer_size) {
      TSDebug(DEBUG_TAG,
              "The request size %lu is larger than memory buffer size %lu, bypass upload proxy feature for this request.",
              my_state->req_size, uconfig->mem_buffer_size);

      pvc_cleanup(new_cont, my_state);
      TSHandleMLocRelease(req_bufp, req_loc, field_loc);
      TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
      break;
    }

    if (TSContDataSet(new_cont, my_state) == TS_ERROR) {
      LOG_ERROR("TSContDataSet");

      pvc_cleanup(new_cont, my_state);
      TSHandleMLocRelease(req_bufp, req_loc, field_loc);
      TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
      break;
    }

    if (uconfig->use_disk_buffer) {
      char path[500];
      int index = (int) (random() % uconfig->subdir_num);

      sprintf(path, "%s/%02X", uconfig->base_dir, index);

      /* 
       * Possible issue with tempnam:
       * From: http://www.gnu.org/s/hello/manual/libc/Temporary-Files.html
       * Warning: Between the time the pathname is constructed and the 
       * file is created another process might have created a file with 
       * the same name using tempnam, leading to a possible security 
       * hole. The implementation generates names which can hardly be 
       * predicted, but when opening the file you should use the O_EXCL 
       * flag. Using tmpfile or mkstemp is a safe way to avoid this problem.
       */

      my_state->filename = tempnam(path, NULL);
      TSDebug(DEBUG_TAG, "temp filename: %s", my_state->filename);

      my_state->fd = open(my_state->filename, O_RDWR | O_NONBLOCK | O_TRUNC | O_CREAT);
      if (my_state->fd < 0) {
        LOG_ERROR("open");
        uconfig->use_disk_buffer = 0;
        my_state->fd = -1;
      }
    }


    TSDebug(DEBUG_TAG, "calling TSHttpTxnIntercept() ...");
    if (TSHttpTxnIntercept(new_cont, txnp) == TS_ERROR) {
      LOG_ERROR("TSHttpTxnIntercept");

      pvc_cleanup(new_cont, my_state);
      TSHandleMLocRelease(req_bufp, req_loc, field_loc);
      TSHandleMLocRelease(req_bufp, TS_NULL_MLOC, req_loc);
      break;
    }

    break;
  default:
    TSReleaseAssert(!"Unexpected Event");
    break;
  }

  if (TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE) == TS_ERROR) {
    LOG_ERROR_AND_RETURN("TSHttpTxnReenable");
  }

  return 0;
}

static int
create_directory()
{
  char str[10];
  char cwd[4096];
  int i;
  DIR *dir;
  struct dirent *d;
  struct passwd *pwd;

  if (getcwd(cwd, 4096) == NULL) {
    TS_ERROR("getcwd fails");
    return 0;
  }

  if ((pwd = getpwnam("nobody")) == NULL) {
    TS_ERROR("can't get passwd entry for \"nobody\"");
    goto error_out;
  }

  if (chdir(uconfig->base_dir) < 0) {
    if (mkdir(uconfig->base_dir, S_IRWXU | S_IRWXG | S_IRWXO) < 0) {
      TS_ERROR("Unable to enter or create %s", uconfig->base_dir);
      goto error_out;
    }
    if (chown(uconfig->base_dir, pwd->pw_uid, pwd->pw_gid) < 0) {
      TS_ERROR("Unable to chown %s", uconfig->base_dir);
      goto error_out;
    }
    if (chdir(uconfig->base_dir) < 0) {
      TS_ERROR("Unable enter %s", uconfig->base_dir);
      goto error_out;
    }
  }
  for (i = 0; i < uconfig->subdir_num; i++) {
    snprintf(str, 10, "%02X", i);
    if (chdir(str) < 0) {
      if (mkdir(str, S_IRWXU | S_IRWXG | S_IRWXO) < 0) {
        TS_ERROR("Unable to enter or create %s/%s", uconfig->base_dir, str);
        goto error_out;
      }
      if (chown(str, pwd->pw_uid, pwd->pw_gid) < 0) {
        TS_ERROR("Unable to chown %s", str);
        goto error_out;
      }
      if (chdir(str) < 0) {
        TS_ERROR("Unable to enter %s/%s", uconfig->base_dir, str);
        goto error_out;
      }
    }
    dir = opendir(".");
    while (d = readdir(dir)) {
      remove(d->d_name);
    }
    chdir("..");
  }

  chdir(cwd);
  return 1;

error_out:
  chdir(cwd);
  return 0;

}

static void
load_urls(char *filename)
{
  TSFile file;
  char *url_buf;
  char *eol;
  int i;

  url_buf = (char *) TSmalloc(sizeof(char) * (uconfig->max_url_length + 1));
  url_buf[uconfig->max_url_length] = '\0';

  for (i = 0; i < 2; i++) {
    if ((file = TSfopen(filename, "r")) == NULL) {
      TSfree(url_buf);
      TS_ERROR("Fail to open %s", filename);
      return;
    }
    if (i == 0) {               //first round
      uconfig->url_num = 0;
      while (TSfgets(file, url_buf, uconfig->max_url_length) != NULL) {
        uconfig->url_num++;
      }
      uconfig->urls = (char **) TSmalloc(sizeof(char *) * uconfig->url_num);
    } else {                    //second round
      int idx = 0;
      while (TSfgets(file, url_buf, uconfig->max_url_length) != NULL && idx < uconfig->url_num) {
        if ((eol = strstr(url_buf, "\r\n")) != NULL) {
          /* To handle newlines on Windows */
          *eol = '\0';
        } else if ((eol = strchr(url_buf, '\n')) != NULL) {
          *eol = '\0';
        } else {
          /* Not a valid line, skip it */
          continue;
        }
        uconfig->urls[idx] = TSstrdup(url_buf);
        idx++;
      }
      uconfig->url_num = idx;
    }
    TSfclose(file);
  }
  TSfree(url_buf);
}


void
parse_config_line(char *line, const struct config_val_ul *cv)
{
  const char *delim = "\t\r\n ";
  char *save = NULL;
  char *tok = strtok_r(line, delim, &save);

  if (tok != NULL) {
    while (cv->str) {
      if (!strcmp(tok, cv->str)) {
        tok = strtok_r(NULL, delim, &save);
        if (tok) {
          switch (cv->type) {
          case TYPE_INT:{
              char *end = tok;
              int iv = strtol(tok, &end, 10);
              if (end && *end == '\0') {
                *((int *) cv->val) = iv;
                TS_ERROR("Parsed int config value %s : %d", cv->str, iv);
                TSDebug(DEBUG_TAG, "Parsed int config value %s : %d", cv->str, iv);
              }
              break;
            }
          case TYPE_UINT:{
              char *end = tok;
              unsigned int uiv = strtoul(tok, &end, 10);
              if (end && *end == '\0') {
                *((unsigned int *) cv->val) = uiv;
                TS_ERROR("Parsed uint config value %s : %u", cv->str, uiv);
                TSDebug(DEBUG_TAG, "Parsed uint config value %s : %u", cv->str, uiv);
              }
              break;
            }
          case TYPE_LONG:{
              char *end = tok;
              long lv = strtol(tok, &end, 10);
              if (end && *end == '\0') {
                *((long *) cv->val) = lv;
                TS_ERROR("Parsed long config value %s : %ld", cv->str, lv);
                TSDebug(DEBUG_TAG, "Parsed long config value %s : %ld", cv->str, lv);
              }
              break;
            }
          case TYPE_ULONG:{
              char *end = tok;
              unsigned long ulv = strtoul(tok, &end, 10);
              if (end && *end == '\0') {
                *((unsigned long *) cv->val) = ulv;
                TS_ERROR("Parsed ulong config value %s : %lu", cv->str, ulv);
                TSDebug(DEBUG_TAG, "Parsed ulong config value %s : %lu", cv->str, ulv);
              }
              break;
            }
          case TYPE_STRING:{
              size_t len = strlen(tok);
              if (len > 0) {
                *((char **) cv->val) = (char *) TSmalloc(len + 1);
                strcpy(*((char **) cv->val), tok);
                TS_ERROR("Parsed string config value %s : %s", cv->str, tok);
                TSDebug(DEBUG_TAG, "Parsed string config value %s : %s", cv->str, tok);
              }
              break;
            }
          case TYPE_BOOL:{
              size_t len = strlen(tok);
              if (len > 0) {
                if (*tok == '1' || *tok == 't')
                  *((bool *) cv->val) = true;
                else
                  *((bool *) cv->val) = false;
                TS_ERROR("Parsed bool config value %s : %d", cv->str, *((bool *) cv->val));
                TSDebug(DEBUG_TAG, "Parsed bool config value %s : %d", cv->str, *((bool *) cv->val));
              }
              break;
            }
          default:
            break;
          }
        }
      }
      cv++;
    }
  }
}

bool
read_upload_config(const char *file_name)
{
  TSDebug(DEBUG_TAG, "read_upload_config: %s", file_name);
  uconfig = (upload_config *) TSmalloc(sizeof(upload_config));
  uconfig->use_disk_buffer = true;
  uconfig->convert_url = false;
  uconfig->chunk_size = 16 * 1024;
  uconfig->mem_buffer_size = 32 * 1024;
  uconfig->url_list_file = NULL;
  uconfig->max_url_length = 4096;
  uconfig->url_num = 0;
  uconfig->urls = NULL;
  uconfig->base_dir = NULL;
  uconfig->subdir_num = 64;
  uconfig->thread_num = 4;

  struct config_val_ul config_vals[] = {
    {"use_disk_buffer", TYPE_BOOL, &(uconfig->use_disk_buffer)},
    {"convert_url", TYPE_BOOL, &(uconfig->convert_url)},
    {"chunk_size", TYPE_ULONG, &(uconfig->chunk_size)},
    {"mem_buffer_size", TYPE_ULONG, &(uconfig->mem_buffer_size)},
    {"url_list_file", TYPE_STRING, &(uconfig->url_list_file)},
    {"max_url_length", TYPE_ULONG, &(uconfig->max_url_length)},
    {"base_dir", TYPE_STRING, &(uconfig->base_dir)},
    {"subdir_num", TYPE_UINT, &(uconfig->subdir_num)},
    {"thread_num", TYPE_UINT, &(uconfig->thread_num)},
    {NULL, TYPE_LONG, NULL}
  };
  TSFile conf_file;
  conf_file = TSfopen(file_name, "r");

  if (conf_file != NULL) {
    TSDebug(DEBUG_TAG, "opened config: %s", file_name);
    char buf[1024];
    while (TSfgets(conf_file, buf, sizeof(buf) - 1) != NULL) {
      if (buf[0] != '#') {
        parse_config_line(buf, config_vals);
      }
    }
    TSfclose(conf_file);
  } else {
    TS_ERROR("Failed to open upload config file %s", file_name);
    // if fail to open config file, use the default config
  }

  if (uconfig->base_dir == NULL) {
    uconfig->base_dir = TSstrdup("/FOOBAR/var/buffer_upload_tmp");
  } else {
    // remove the "/" at the end.
    if (uconfig->base_dir[strlen(uconfig->base_dir) - 1] == '/') {
      uconfig->base_dir[strlen(uconfig->base_dir) - 1] = '\0';
    }
  }

  if (uconfig->subdir_num <= 0) {
    // default value
    uconfig->subdir_num = 64;
  }

  if (uconfig->thread_num <= 0) {
    // default value
    uconfig->thread_num = 4;
  }
  return true;
}

void
TSPluginInit(int argc, const char *argv[])
{
  LOG_SET_FUNCTION_NAME("TSPluginInit");

  TSMLoc field_loc;
  const char *p;
  int i;
  int c;
  TSPluginRegistrationInfo info;
  TSCont contp;
  char default_filename[1024];
  const char *conf_filename;

  if (argc > 1) {
    conf_filename = argv[1];
  } else {
    sprintf(default_filename, "%s/upload.conf", TSPluginDirGet());
    conf_filename = default_filename;
  }

  if (!read_upload_config(conf_filename) || !uconfig) {
    if (argc > 1) {
      TS_ERROR("Failed to read upload config %s\n", argv[1]);
    } else {
      TS_ERROR("No config file specified. Specify conf file in plugin.conf: "
               "'buffer_upload.so /path/to/upload.conf'\n");
    }
  }
  // set the num of threads for disk AIO
  if (TSAIOThreadNumSet(uconfig->thread_num) == TS_ERROR) {
    TS_ERROR("Failed to set thread number.");
  }

  TSDebug(DEBUG_TAG, "uconfig->url_list_file: %s", uconfig->url_list_file);
  if (uconfig->url_list_file) {
    load_urls(uconfig->url_list_file);
    TSDebug(DEBUG_TAG, "loaded uconfig->url_list_file, num urls: %d", uconfig->url_num);
  }

  info.plugin_name = "buffer_upload";
  info.vendor_name = "";
  info.support_email = "";

  if (uconfig->use_disk_buffer && !create_directory()) {
    TS_ERROR("Directory creation failed.");
    uconfig->use_disk_buffer = 0;
  }

  if (!TSPluginRegister(TS_SDK_VERSION_2_0, &info)) {
    TS_ERROR("Plugin registration failed.");
  }

  /* create the statistic variables */
  upload_vc_count = TSStatCreate("upload_vc.count", TSSTAT_TYPE_INT64);
  if (upload_vc_count == TS_ERROR_PTR) {
    LOG_ERROR("TSStatsCreate");
  }

  contp = TSContCreate(attach_pvc_plugin, NULL);
  if (contp == TS_ERROR_PTR) {
    LOG_ERROR("TSContCreate");
  } else {
    if (TSHttpHookAdd(TS_HTTP_READ_REQUEST_PRE_REMAP_HOOK, contp) == TS_ERROR) {
      LOG_ERROR("TSHttpHookAdd");
    }
  }
}