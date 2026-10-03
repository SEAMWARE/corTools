//
// FILE            corRequest.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corRequest - a cor:// client for tests and benchmarks: what curl and wrk are for HTTP
//
//   one request, like curl -i:
//     corRequest --url cor://localhost:2026 --path /ngsi-ld/v1/entities/urn:E1
//     corRequest --url cor://localhost:2026 --verb POST --path /ngsi-ld/v1/entities --payload '{...}'
//     prints "STATUS <code>", the headers, an empty line and the body as JSON; exit 0 when a
//     response came back (whatever its status), 1 when none did
//
//   load, like wrk:
//     corRequest --url cor://localhost:2026 --path ... -c 16 --duration 8
//     <c> connections, each a thread of its own sending one request after the other for <d>
//     seconds; prints requests/second and the p50 / p90 / p99 / max latency
//
//   load at a fixed rate, like wrk2:
//     corRequest --url http://localhost:1026 --path ... -c 16 --rate 30000 --duration 8
//     each connection sends at its share of <rate>, on a schedule; a latency counts from the moment
//     the request was DUE, not from when it went out - a slow answer delays the requests behind it,
//     and they count that delay. Two builds compared at one rate, instead of each at the rate it
//     reaches (which favours the faster build's p99 against itself).
//     An http:// URL loads an HTTP server (GET, keep-alive, Content-Length bodies) - load mode only.
//
// Headers: --header 'Name: value', several separated by '|'.
//
//   as the functests' corCurl, over cor:// instead of curl:
//     corRequest --url ... --path ... --curl --bodyFile /tmp/x.body --pretty 2
//     the header block exactly as curl -D shows the HTTP server's - the status line with its reason
//     phrase, Date, the broker's headers, Content-Length - and the body, as the broker sends it, in
//     bodyFile. corCurl then treats that file as it treats curl's. Content-Length is the size the
//     broker WOULD have rendered: its JSON at --pretty spaces per indent (test brokers run with
//     --pretty-print 2).
//
//   exit: 0 a response; 1 no response; 3 the payload is not JSON - a request only HTTP can carry
//
#define _GNU_SOURCE                                   // strcasestr
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // printf, fprintf
#include <errno.h>                                    // errno, EINTR
#include <netdb.h>                                    // getaddrinfo
#include <netinet/in.h>                               // IPPROTO_TCP
#include <netinet/tcp.h>                              // TCP_NODELAY
#include <stdlib.h>                                   // malloc, free, qsort, strtol
#include <sys/socket.h>                               // socket, connect, setsockopt
#include <string.h>                                   // strchr, strlen, strtok_r
#include <time.h>                                     // clock_gettime
#include <unistd.h>                                   // usleep
#include <pthread.h>                                  // pthread_create, pthread_join

#include "corArgs/corArgs.h"                          // corArgsInit, corArgsParse
#include "corArgs/corArgsBuiltins.h"                  // corArgsBuiltinVerbose
#include "corLog/corLog.h"                            // corLogInit
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonRender.h"                    // corJsonRender
#include "corJson/corJsonRenderSize.h"                // corJsonRenderSize, corJsonFastRenderSize
#include "corRest/CorRestVerb.h"                      // CorRestVerb, corRestVerbFromString
#include "corRest/CorRestKeyValue.h"                  // CorRestKeyValue
#include "corBase/corFileRead.h"                       // corFileRead
#include "corRest/corRestCor.h"                       // corRestCorInit, corRestCorSend
#include "corNgsild/ldBinCodec.h"                     // ldBinCodec, ldBinNamespaceV
#include "corNgsild/CorTerm.h"                        // CorTermLast



// -----------------------------------------------------------------------------
//
// Options
//
static char*         url          = NULL;
static char*         path         = (char*) "/ngsi-ld/v1/entities";
static char*         verb         = (char*) "GET";
static char*         payload      = NULL;
static char*         payloadFile  = NULL;
static char*         paths        = NULL;
static char*         headers      = NULL;
static unsigned int  connections  = 0;
static unsigned int  seconds      = 8;
static unsigned int  timeoutMs    = 30000;
static bool          curlMode     = false;
static char*         bodyFile     = NULL;
static int           pretty       = 0;
static unsigned int  rate         = 0;
static unsigned int  sockets      = 0;

static CorArg argV[] =
{
  { "--url",     NULL, CorArgString, _vp &url,         CorArgReq, NULL,              NULL,   NULL,          "cor://host:port" },
  { "--path",    NULL, CorArgString, _vp &path,        CorArgOpt, _vp "/ngsi-ld/v1/entities", NULL, NULL, "path and query" },
  { "--verb",    "-X", CorArgString, _vp &verb,        CorArgOpt, _vp "GET",         NULL,   NULL,          "GET, POST, PATCH, PUT, DELETE" },
  { "--payload", NULL, CorArgString, _vp &payload,     CorArgOpt, NULL,              NULL,   NULL,          "request body (JSON)" },
  { "--payloadFile", NULL, CorArgString, _vp &payloadFile, CorArgOpt, NULL,          NULL,   NULL,          "request body (JSON), from a file - for a body too large for the command line" },
  { "--header",  "-H", CorArgString, _vp &headers,     CorArgOpt, NULL,              NULL,   NULL,          "'Name: value', several separated by '|'" },
  { "--conns",   "-c", CorArgUInt,   _vp &connections, CorArgOpt, _vp 0,             _vp 0,  _vp 1024,      "load mode: connections (0: one request)" },
  { "--paths",   NULL, CorArgString, _vp &paths,       CorArgOpt, NULL,              NULL,   NULL,          "parallel mode: 'path|path|...', each sent at once, answers printed as they come" },
  { "--duration",NULL, CorArgUInt,   _vp &seconds,     CorArgOpt, _vp 8,             _vp 1,  _vp 3600,      "load mode: seconds" },
  { "--sockets", NULL, CorArgUInt,   _vp &sockets,     CorArgOpt, _vp 0,             _vp 0,  _vp 1,         "parallel mode: 1 = every path on ONE connection (multiplexed), printed in the order the answers came; 0 = a thread and a connection each" },
  { "--rate",    NULL, CorArgUInt,   _vp &rate,        CorArgOpt, _vp 0,             _vp 0,  _vp 10000000,  "load mode: requests/second in all, on a schedule (0: as fast as answered)" },
  { "--timeout", NULL, CorArgUInt,   _vp &timeoutMs,   CorArgOpt, _vp 30000,         _vp 1,  _vp 600000,    "milliseconds" },
  { "--curl",    NULL, CorArgBool,   _vp &curlMode,    CorArgOpt, _vp false,         _vp false, _vp true,   "print as the functests' corCurl does (with --bodyFile)" },
  { "--bodyFile",NULL, CorArgString, _vp &bodyFile,    CorArgOpt, NULL,              NULL,   NULL,          "--curl: where the body goes" },
  { "--pretty",  NULL, CorArgInt,    _vp &pretty,      CorArgOpt, _vp 0,             _vp 0,  _vp 16,        "--curl: the broker's --pretty-print, for Content-Length" },
  CORARGS_END
};



// -----------------------------------------------------------------------------
//
// headerV - the --header option, split
//
static CorRestKeyValue headerV[32];
static int             headerCount = 0;

static void headersSplit(void)
{
  if (headers == NULL)
    return;

  char* save = NULL;

  for (char* h = strtok_r(headers, "|", &save); (h != NULL) && (headerCount < 32); h = strtok_r(NULL, "|", &save))
  {
    char* colon = strchr(h, ':');

    if (colon == NULL)
      continue;

    *colon = 0;
    char* value = &colon[1];
    while (*value == ' ')
      ++value;

    headerV[headerCount].key   = h;
    headerV[headerCount].value = value;
    headerCount += 1;
  }
}



// -----------------------------------------------------------------------------
//
// once - one request, printed like curl -i
//
static int once(CorRestVerb v)
{
  CorAlloc           ka;
  static char        kaBuf[64 * 1024];
  CorRestCorResponse resp;
  const char*        error = NULL;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 64 * 1024, NULL, "corRequest");

  if (corRestCorSend(url, v, path, headerV, headerCount, NULL, payload, timeoutMs, &ka, &resp, &error) == false)
  {
    fprintf(stderr, "corRequest: %s\n", (error != NULL) ? error : "failed");
    return 1;
  }

  printf("STATUS %d\n", resp.status);
  for (int i = 0; i < resp.headerCount; i++)
    printf("%s: %s\n", resp.headerV[i].key, resp.headerV[i].value);
  printf("\n");

  if (resp.bodyTree != NULL)
  {
    CorJson cj;

    corJsonCreate(&cj, &ka);
    char* out = corAlloc(&ka, corJsonRenderSize(&cj, resp.bodyTree) + 1);
    corJsonRender(&cj, resp.bodyTree, out);
    printf("%s\n", out);
  }
  else if (resp.bodyText != NULL)
    printf("%s\n", resp.bodyText);

  corAllocBufferReset(&ka, false);
  return 0;
}



// -----------------------------------------------------------------------------
//
// reasonPhrase - the HTTP server's own (libmicrohttpd's) - the functests compare it
//
static const char* reasonPhrase(int status)
{
  switch (status)
  {
  case 100: return "Continue";
  case 200: return "OK";
  case 201: return "Created";
  case 202: return "Accepted";
  case 204: return "No Content";
  case 207: return "Multi-Status";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 304: return "Not Modified";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 406: return "Not Acceptable";
  case 409: return "Conflict";
  case 411: return "Length Required";
  case 412: return "Precondition Failed";
  case 413: return "Content Too Large";
  case 415: return "Unsupported Media Type";
  case 422: return "Unprocessable Content";
  case 429: return "Too Many Requests";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  case 508: return "Loop Detected";
  default:  return "Unknown";
  }
}



// -----------------------------------------------------------------------------
//
// curlOnce - one request, shown as corCurl shows an HTTP one
//
// The HTTP server writes Date first and Content-Length last, around the broker's own headers - which
// cor:// carries in the same order. A 204 has no Content-Length.
//
static int curlOnce(CorRestVerb v)
{
  CorAlloc           ka;
  static char        kaBuf[64 * 1024];
  CorRestCorResponse resp;
  const char*        error = NULL;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 256 * 1024, NULL, "corRequest");

  if (corRestCorSend(url, v, path, headerV, headerCount, NULL, payload, timeoutMs, &ka, &resp, &error) == false)
  {
    if ((error != NULL) && (strcmp(error, "the request body is not JSON") == 0))
      return 3;

    fprintf(stderr, "corRequest: %s\n", (error != NULL) ? error : "failed");
    return 1;
  }

  //
  // The body, as the broker would have rendered it for HTTP
  //
  char* body    = NULL;
  int   bodyLen = 0;

  if (resp.bodyTree != NULL)
  {
    CorJson cj;

    corJsonCreate(&cj, &ka);
    if (pretty > 0)
    {
      cj.spacesPerIndent = pretty;
      body = corAlloc(&ka, corJsonRenderSize(&cj, resp.bodyTree) + 1);
      corJsonRender(&cj, resp.bodyTree, body);
    }
    else
    {
      body = corAlloc(&ka, corJsonFastRenderSize(resp.bodyTree) + 1);
      corJsonFastRender(resp.bodyTree, body);
    }
    bodyLen = strlen(body);
  }
  else if (resp.bodyText != NULL)
  {
    body    = resp.bodyText;
    bodyLen = strlen(body);
  }

  char      date[64];
  time_t    now = time(NULL);
  struct tm tm;

  gmtime_r(&now, &tm);
  strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);

  printf("HTTP/1.1 %d %s\n", resp.status, reasonPhrase(resp.status));
  printf("Date: %s\n", date);
  for (int i = 0; i < resp.headerCount; i++)
    printf("%s: %s\n", resp.headerV[i].key, resp.headerV[i].value);
  if (resp.status != 204)
    printf("Content-Length: %d\n", bodyLen);

  if (bodyFile != NULL)
  {
    FILE* fP = fopen(bodyFile, "w");

    if (fP != NULL)
    {
      if (bodyLen > 0)
        fwrite(body, 1, bodyLen, fP);
      fclose(fP);
    }
  }

  corAllocBufferReset(&ka, false);
  return 0;
}


// -----------------------------------------------------------------------------
//
// Parallel mode - every --paths entry sent at the same time, each on a thread - and a connection - of its own
//
// One thread per path, the i-th sent i x 100 ms after the first - the order they go out in is the
// order given. Each answer is printed when it arrives - "<status> <path>" - so the order of the lines
// is the order the responses came back in: over one multiplexed connection, a fast request sent
// after a slow one is answered first.
//
typedef struct Parallel
{
  pthread_t        tid;
  CorRestVerb      verb;
  const char*      path;
  int              index;
} Parallel;

static pthread_mutex_t printMutex = PTHREAD_MUTEX_INITIALIZER;

static void* parallelOne(void* arg)
{
  Parallel*          pP = (Parallel*) arg;
  CorAlloc           ka;
  char               kaBuf[16 * 1024];
  CorRestCorResponse resp;
  const char*        error;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 64 * 1024, NULL, "corRequest parallel");
  usleep(pP->index * 100000);

  bool ok = corRestCorSend(url, pP->verb, pP->path, headerV, headerCount, NULL, payload, timeoutMs, &ka, &resp, &error);

  pthread_mutex_lock(&printMutex);
  if (ok == true)
    printf("%d %s\n", resp.status, pP->path);
  else
    printf("ERROR %s: %s\n", pP->path, error);
  fflush(stdout);
  pthread_mutex_unlock(&printMutex);

  corAllocBufferReset(&ka, false);
  return NULL;
}

// -----------------------------------------------------------------------------
//
// parallelOneSocket - --sockets 1: every path started on this thread - one connection, multiplexed -
// 100 ms apart, then all waited for; printed in the order the answers ARRIVED, not the order sent
//
typedef struct OneSocket
{
  const char*         path;
  CorRestCorCall*     callP;
  CorRestCorResponse  resp;
  bool                ok;
  const char*         error;
} OneSocket;

static int parallelOneSocket(CorRestVerb v, Parallel* pV, int n)
{
  static char  kaBuf[64 * 1024];
  CorAlloc     ka;
  OneSocket    oV[32];

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 64 * 1024, NULL, "corRequest one socket");

  for (int i = 0; i < n; i++)
  {
    if (i > 0)
      usleep(100000);

    oV[i].path  = pV[i].path;
    oV[i].error = NULL;
    oV[i].callP = corRestCorStart(url, v, pV[i].path, headerV, headerCount, NULL, payload, timeoutMs, &ka, &oV[i].error);
  }

  for (int i = 0; i < n; i++)
  {
    memset(&oV[i].resp, 0, sizeof(oV[i].resp));
    oV[i].ok = (oV[i].callP != NULL) && (corRestCorWait(oV[i].callP, &oV[i].resp, &oV[i].error) == true);
  }

  for (int printed = 0; printed < n; printed++)       // the earliest arrival first; failures last, in order
  {
    int best = -1;

    for (int i = 0; i < n; i++)
    {
      if (oV[i].path == NULL)
        continue;
      if ((best == -1) || ((oV[i].ok == true) && ((oV[best].ok == false) || (oV[i].resp.receivedMs < oV[best].resp.receivedMs))))
        best = i;
    }

    if (oV[best].ok == true)
      printf("%d %s\n", oV[best].resp.status, oV[best].path);
    else
      printf("ERROR %s: %s\n", oV[best].path, (oV[best].error != NULL) ? oV[best].error : "failed");
    oV[best].path = NULL;
  }

  corAllocBufferReset(&ka, false);
  return 0;
}

static int parallel(CorRestVerb v)
{
  Parallel  pV[32];
  int       n    = 0;
  char*     save = NULL;

  for (char* p = strtok_r(paths, "|", &save); (p != NULL) && (n < 32); p = strtok_r(NULL, "|", &save))
  {
    pV[n].verb  = v;
    pV[n].path  = p;
    pV[n].index = n;
    n += 1;
  }

  if (sockets == 1)
    return parallelOneSocket(v, pV, n);

  for (int i = 0; i < n; i++)
    pthread_create(&pV[i].tid, NULL, parallelOne, &pV[i]);
  for (int i = 0; i < n; i++)
    pthread_join(pV[i].tid, NULL);

  return 0;
}



// -----------------------------------------------------------------------------
//
// Load mode
//
typedef struct Worker
{
  pthread_t  tid;
  CorRestVerb verb;
  double     endAt;
  long       done;
  long       errors;
  double*    latV;        // microseconds
  long       latMax;
  double     interval;    // --rate: seconds between this connection's requests; 0: none
  int        httpFd;      // an http:// URL: this connection's socket
  char*      httpBuf;
} Worker;

static char  httpHost[256];
static char  httpPort[16];
static char* httpRequest    = NULL;
static int   httpRequestLen = 0;
enum { HTTP_BUF = 1024 * 1024 };



// -----------------------------------------------------------------------------
//
// httpUrl - http://host[:port]: host and port, and the request every GET sends; false: not http://
//
static bool httpUrl(void)
{
  if (strncmp(url, "http://", 7) != 0)
    return false;

  const char* hostP = url + 7;
  const char* colon = strchr(hostP, ':');
  const char* slash = strchr(hostP, '/');
  int         hLen  = (int) ((colon != NULL) ? colon - hostP : (slash != NULL) ? slash - hostP : (long) strlen(hostP));

  snprintf(httpHost, sizeof(httpHost), "%.*s", hLen, hostP);
  if (colon != NULL)
    snprintf(httpPort, sizeof(httpPort), "%ld", strtol(colon + 1, NULL, 10));
  else
    snprintf(httpPort, sizeof(httpPort), "80");

  int size = 1024 + (int) strlen(path);
  for (int i = 0; i < headerCount; i++)
    size += (int) (strlen(headerV[i].key) + strlen(headerV[i].value) + 4);

  httpRequest    = malloc(size);
  httpRequestLen = snprintf(httpRequest, size, "GET %s HTTP/1.1\r\nHost: %s:%s\r\n", path, httpHost, httpPort);
  for (int i = 0; i < headerCount; i++)
    httpRequestLen += snprintf(httpRequest + httpRequestLen, size - httpRequestLen, "%s: %s\r\n", headerV[i].key, headerV[i].value);
  httpRequestLen += snprintf(httpRequest + httpRequestLen, size - httpRequestLen, "\r\n");

  return true;
}



// -----------------------------------------------------------------------------
//
// httpConnect -
//
static int httpConnect(void)
{
  struct addrinfo  hints;
  struct addrinfo* aiP = NULL;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(httpHost, httpPort, &hints, &aiP) != 0)
    return -1;

  int fd = socket(aiP->ai_family, aiP->ai_socktype, 0);

  if ((fd >= 0) && (connect(fd, aiP->ai_addr, aiP->ai_addrlen) != 0))
  {
    close(fd);
    fd = -1;
  }
  freeaddrinfo(aiP);

  if (fd >= 0)
  {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  }

  return fd;
}



// -----------------------------------------------------------------------------
//
// httpGet - one GET on the worker's connection (opened, or reopened, as needed); the status, -1: none
//
static int httpGet(Worker* wP)
{
  if ((wP->httpFd < 0) && ((wP->httpFd = httpConnect()) < 0))
    return -1;

  for (int sent = 0; sent < httpRequestLen; )
  {
    ssize_t n = write(wP->httpFd, httpRequest + sent, httpRequestLen - sent);

    if ((n < 0) && (errno == EINTR))
      continue;
    if (n <= 0)
      goto broken;
    sent += (int) n;
  }

  int   got     = 0;
  int   status  = -1;
  long  total   = -1;                                // header + body, once the header is in

  while ((total < 0) || (got < total))
  {
    if (got >= HTTP_BUF - 1)
      goto broken;

    ssize_t n = read(wP->httpFd, wP->httpBuf + got, HTTP_BUF - 1 - got);

    if ((n < 0) && (errno == EINTR))
      continue;
    if (n <= 0)
      goto broken;

    got += (int) n;
    wP->httpBuf[got] = 0;

    if (total < 0)
    {
      char* end = strstr(wP->httpBuf, "\r\n\r\n");

      if (end == NULL)
        continue;

      status = (int) strtol(wP->httpBuf + 9, NULL, 10);   // "HTTP/1.1 200"

      long  len = 0;
      char* clP = strcasestr(wP->httpBuf, "\r\nContent-Length:");
      if ((clP != NULL) && (clP < end))
        len = strtol(clP + 17, NULL, 10);

      total = (end + 4 - wP->httpBuf) + len;
    }
  }

  return status;

broken:
  close(wP->httpFd);
  wP->httpFd = -1;
  return -1;
}

static double now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void* worker(void* arg)
{
  Worker*            wP = (Worker*) arg;
  CorAlloc           ka;
  char*              kaBuf = malloc(64 * 1024);
  CorRestCorResponse resp;
  const char*        error;

  corAllocBufferInit(&ka, kaBuf, 64 * 1024, 64 * 1024, NULL, "corRequest load");

  double due = now();

  for (double t0 = due; t0 < wP->endAt; t0 = now())
  {
    if (wP->interval > 0)
    {
      //
      // On the schedule: wait for the request's due time, and count its latency from then - a request
      // already late (the previous answer took longer than the interval) goes at once, its lateness
      // counted
      //
      while (t0 < due)
      {
        struct timespec ts;
        double          wait = due - t0;

        ts.tv_sec  = (time_t) wait;
        ts.tv_nsec = (long) ((wait - ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
        t0 = now();
      }

      t0   = due;
      due += wP->interval;

      if (t0 >= wP->endAt)
        break;
    }

    bool ok;

    if (httpRequest != NULL)
    {
      int status = httpGet(wP);
      ok          = (status > 0);
      resp.status = status;
    }
    else
      ok = corRestCorSend(url, wP->verb, path, headerV, headerCount, NULL, payload, timeoutMs, &ka, &resp, &error);

    if ((ok == true) && (resp.status < 400))
    {
      if (wP->done < wP->latMax)
        wP->latV[wP->done] = (now() - t0) * 1e6;
      wP->done += 1;
    }
    else
      wP->errors += 1;

    corAllocBufferReset(&ka, true);                  // reuse - a reset in a loop must not free the buffer
  }

  corAllocBufferReset(&ka, false);
  free(kaBuf);
  return NULL;
}

static int doubleCompare(const void* a, const void* b)
{
  double x = *(const double*) a;
  double y = *(const double*) b;

  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static int load(CorRestVerb v)
{
  Worker* wV     = calloc(connections, sizeof(Worker));
  long    latMax = 4000000 / connections + 1000;
  double  start  = now();

  for (unsigned int i = 0; i < connections; i++)
  {
    wV[i].verb   = v;
    wV[i].endAt  = start + seconds;
    wV[i].latMax = latMax;
    wV[i].latV   = malloc(latMax * sizeof(double));
    wV[i].httpFd   = -1;
    wV[i].httpBuf  = (httpRequest != NULL) ? malloc(HTTP_BUF) : NULL;
    wV[i].interval = (rate > 0) ? (double) connections / rate : 0;
    pthread_create(&wV[i].tid, NULL, worker, &wV[i]);
  }

  long done = 0, errors = 0, kept = 0;

  for (unsigned int i = 0; i < connections; i++)
  {
    pthread_join(wV[i].tid, NULL);
    done   += wV[i].done;
    errors += wV[i].errors;
    kept   += (wV[i].done < latMax) ? wV[i].done : latMax;
  }

  double  elapsed = now() - start;
  double* allV    = malloc((kept + 1) * sizeof(double));
  long    n       = 0;

  for (unsigned int i = 0; i < connections; i++)
  {
    long k = (wV[i].done < latMax) ? wV[i].done : latMax;

    memcpy(&allV[n], wV[i].latV, k * sizeof(double));
    n += k;
    free(wV[i].latV);
    free(wV[i].httpBuf);
    if (wV[i].httpFd >= 0)
      close(wV[i].httpFd);
  }

  qsort(allV, n, sizeof(double), doubleCompare);

  #define PCT(p) ((n > 0) ? allV[(long) ((n - 1) * (p))] : 0.0)
  printf("%u connections, %u s: %ld requests, %ld errors, %.2f req/s  |  p50 %.0f us  p90 %.0f us  p99 %.0f us  max %.0f us\n",
         connections, seconds, done, errors, done / elapsed, PCT(0.50), PCT(0.90), PCT(0.99), (n > 0) ? allV[n - 1] : 0.0);

  free(allV);
  free(wV);
  return (errors == 0) ? 0 : 1;
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(int argC, char* argV_[])
{
  if ((corArgsInit("corRequest", argV, "CORREQUEST") != CorArgsOk) || (corArgsParse(argC, argV_) != CorArgsOk))
    return 2;

  if (corLogInit("corRequest", "/tmp", false, NULL, "0-255", corArgsBuiltinVerbose, corArgsBuiltinDebug, false) != 0)
    return 2;

  headersSplit();

  if (payloadFile != NULL)
  {
    int len;

    if (corFileRead((char*) "", payloadFile, &payload, &len) != 0)
    {
      fprintf(stderr, "corRequest: cannot read %s\n", payloadFile);
      return 2;
    }
  }
  corRestCorInit(&ldBinCodec, ldBinNamespaceV, ldBinNamespaces, CorTermLast);

  CorRestVerb v = corRestVerbFromString(verb);

  if (paths != NULL)
    return parallel(v);

  if (httpUrl() == true)
  {
    if ((connections == 0) || (v != CorVerbGet))
    {
      fprintf(stderr, "corRequest: an http:// URL is for load mode (-c), GET only\n");
      return 2;
    }
  }

  if (connections > 0)
    return load(v);

  return (curlMode == true) ? curlOnce(v) : once(v);
}
