//
// FILE            corMongoDrop.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corMongoDrop - drop MongoDB databases or collections, fast
//
// What the functests did with mongosh, a JavaScript runtime started for every drop - ~0.3 s each, and
// the suite drops before almost every test. This is one libmongoc connection and a command or two.
//
//   corMongoDrop [--host H] [--port P] --db NAME                      drop database NAME
//   corMongoDrop [--host H] [--port P] --db NAME --collections a,b,c  drop those collections of NAME
//   corMongoDrop [--host H] [--port P] --prefix P                     drop every database named P, or P-...
//   corMongoDrop [--host H] [--port P] --prefix P --contains S        ... whose name also contains S
//
// Exit 0 when it could talk to the server (a database or collection that did not exist is not an
// error - dropping it is a no-op), 1 when it could not, 2 for bad arguments.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // fprintf, snprintf
#include <stdlib.h>                                   // atoi
#include <string.h>                                   // strcmp, strncmp, strstr, strtok_r

#include <mongoc/mongoc.h>                            // mongoc_*



// -----------------------------------------------------------------------------
//
// usage -
//
static int usage(const char* why)
{
  fprintf(stderr, "corMongoDrop: %s\n"
                  "usage: corMongoDrop [--host H] [--port P] --db NAME [--collections a,b,c]\n"
                  "       corMongoDrop [--host H] [--port P] --prefix P [--contains S]\n", why);
  return 2;
}



// -----------------------------------------------------------------------------
//
// prefixMatch - name is P, or starts with "P-"; and contains S if S is given
//
static bool prefixMatch(const char* name, const char* prefix, const char* contains)
{
  size_t pLen = strlen(prefix);

  if (strncmp(name, prefix, pLen) != 0)
    return false;

  if ((name[pLen] != 0) && (name[pLen] != '-'))
    return false;

  return (contains == NULL) || (strstr(name, contains) != NULL);
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(int argC, char* argV[])
{
  const char* host        = "localhost";
  int         port        = 27017;
  const char* db          = NULL;
  char*       collections = NULL;
  const char* prefix      = NULL;
  const char* contains    = NULL;

  for (int i = 1; i < argC; i++)
  {
    const char* opt = argV[i];
    const char* val = (i + 1 < argC) ? argV[i + 1] : NULL;

    if (val == NULL)                               return usage("an option without its value");
    if      (strcmp(opt, "--host")        == 0)    host        = val;
    else if (strcmp(opt, "--port")        == 0)    port        = atoi(val);
    else if (strcmp(opt, "--db")          == 0)    db          = val;
    else if (strcmp(opt, "--collections") == 0)    collections = argV[i + 1];
    else if (strcmp(opt, "--prefix")      == 0)    prefix      = val;
    else if (strcmp(opt, "--contains")    == 0)    contains    = val;
    else                                           return usage("unknown option");
    i++;
  }

  if ((db == NULL) == (prefix == NULL))            return usage("one of --db and --prefix");
  if ((collections != NULL) && (db == NULL))      return usage("--collections goes with --db");
  if ((contains != NULL) && (prefix == NULL))     return usage("--contains goes with --prefix");

  char uri[512];
  snprintf(uri, sizeof(uri), "mongodb://%s:%d/?appname=corMongoDrop&serverSelectionTimeoutMS=5000", host, port);

  mongoc_init();

  bson_error_t     error;
  mongoc_client_t* clientP = mongoc_client_new(uri);
  int              status  = 0;

  if (clientP == NULL)
  {
    fprintf(stderr, "corMongoDrop: bad server address '%s'\n", uri);
    mongoc_cleanup();
    return 2;
  }

  if (db != NULL)
  {
    mongoc_database_t* dbP = mongoc_client_get_database(clientP, db);

    if (collections == NULL)
    {
      if (mongoc_database_drop(dbP, &error) == false)
      {
        fprintf(stderr, "corMongoDrop: dropping '%s': %s\n", db, error.message);
        status = 1;
      }
    }
    else
    {
      char* save = NULL;

      for (char* name = strtok_r(collections, ",", &save); name != NULL; name = strtok_r(NULL, ",", &save))
      {
        mongoc_collection_t* collP = mongoc_database_get_collection(dbP, name);

        // a collection that does not exist answers 'ns not found' (26) - nothing to drop, not an error
        if ((mongoc_collection_drop(collP, &error) == false) && (error.code != 26))
        {
          fprintf(stderr, "corMongoDrop: dropping '%s.%s': %s\n", db, name, error.message);
          status = 1;
        }
        mongoc_collection_destroy(collP);
      }
    }

    mongoc_database_destroy(dbP);
  }
  else
  {
    char** names = mongoc_client_get_database_names_with_opts(clientP, NULL, &error);

    if (names == NULL)
    {
      fprintf(stderr, "corMongoDrop: listing the databases: %s\n", error.message);
      status = 1;
    }
    else
    {
      for (int i = 0; names[i] != NULL; i++)
      {
        if (prefixMatch(names[i], prefix, contains) == false)
          continue;

        mongoc_database_t* dbP = mongoc_client_get_database(clientP, names[i]);

        if (mongoc_database_drop(dbP, &error) == false)
        {
          fprintf(stderr, "corMongoDrop: dropping '%s': %s\n", names[i], error.message);
          status = 1;
        }
        mongoc_database_destroy(dbP);
      }
      bson_strfreev(names);
    }
  }

  mongoc_client_destroy(clientP);
  mongoc_cleanup();

  return status;
}
