#ifndef H_FILE
#define H_FILE

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
using namespace std;

class FILEUTIL {
public:
    static const int PIECE_LEN = 1024;
    FILEUTIL();
    ~FILEUTIL();
    bool MergeFile(const char* name, const int num);

private:
    bool AppendFile(const char* src, const char* aim);

    char* buf;
};
#endif