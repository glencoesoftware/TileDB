/*
 * 
 * Demonstrates how to read from dense array "workspace/A".
 */

#include "c_api.h"
#include <iostream>

int main() {
  /* Intialize context with the default configuration parameters. */
  TileDB_CTX* tiledb_ctx;
  tiledb_ctx_init(&tiledb_ctx, NULL);

  /* Subset over attribute "a1". */
  //const char* attributes[] = { "a1" };
  const char* attributes[] = { TILEDB_KEY_NAME };

  /* Prepare cell buffers for attribute "a1". */
  int buffer_a1[8];
  float buffer_a2[8];
  size_t buffer_key[8];
  char buffer_key_var[500];
  //void* buffers[] = { buffer_a1 };
  //size_t buffer_sizes[] = { sizeof(buffer_a1) };
  void* buffers[] = { buffer_key, buffer_key_var };
  size_t buffer_sizes[] = { sizeof(buffer_key), sizeof(buffer_key_var) };

  /* Initialize the array in READ mode. */
  TileDB_MetadataIterator* tiledb_metadata_iterator;
  tiledb_metadata_iterator_init(
      tiledb_ctx, 
      &tiledb_metadata_iterator,
      "~/.tiledb/master_catalog",
      attributes,           
      1,
      buffers,
      buffer_sizes);      

  /* Read from array. */
  const char* key;
  size_t key_size, a_size;
  const float* a;

  while(!tiledb_metadata_iterator_end(tiledb_metadata_iterator)) {
    tiledb_metadata_iterator_get_value(tiledb_metadata_iterator, 0, (const void**) &key, &key_size);
    if(key[0] == TILEDB_EMPTY_CHAR)
      std::cout << "EMPTY\n";
    else
      std::cout << key << " " << key_size << "\n";
    tiledb_metadata_iterator_next(tiledb_metadata_iterator);
  }

  /* Finalize the metadata iterator. */
  tiledb_metadata_iterator_finalize(tiledb_metadata_iterator);

  /* Finalize context. */
  tiledb_ctx_finalize(tiledb_ctx);

  return 0;
}
