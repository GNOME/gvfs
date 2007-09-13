#include "gfile.h"

int
main (int argc, char *argv[])
{
  GFile *file;

  g_type_init ();
  
  file = g_file_get_for_path ("/tmp");
  g_print ("file: %p\n", file);
  
  return 0;
}
