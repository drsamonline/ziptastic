/* dir/b | mkstring

Reads file names from standard input and prints a C array of
struct initialization of the form:

  {{length, "filename", "contents"}, ..., {0,0,0}};
*/
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace std;

// Read a line of input from f into s, return true if successful
bool getline(FILE* f, string& s) {
  s="";
  int c;
  while ((c=getc(f))!=EOF && c!='\n')
    s+=char(c);
  return !s.empty() || c!=EOF;
}

int main() {

  printf("{");
  string filename;
  while (getline(stdin, filename)) {
    FILE* f=fopen(filename.c_str(), "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long len=ftell(f);
      fseek(f, 0, SEEK_SET);
      printf("{%ld,\"%s\",\n\"", len, filename.c_str());
      int c;
      int pos=1;
      while ((c=getc(f))!=EOF) {
        printf("\\%o", c);
        pos+=2;
        if (c>7) ++pos;
        if (c>63) ++pos;
        if (pos>73) {
          printf("\"\n\"");
          pos=1;
        }
      }
      fclose(f);
      printf("\"},\n");
    }
    else
      perror(filename.c_str());
  }
  printf("{0,0,0}};\n");
  return 0;
}

