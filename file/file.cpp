#include "file.h"

FILEUTIL::FILEUTIL()
{
    buf = (char*)malloc((PIECE_LEN + 5) * sizeof(char));
}

FILEUTIL::~FILEUTIL()
{
    free(buf);
}

bool FILEUTIL::MergeFile(const char* name, const int num)
{
    DIR* dir;
    struct dirent* ptr;
    int count = 0;
    if ((dir = opendir(name)) == NULL) {
        perror("Open dir error.");
        return false;
    }
    while ((ptr = readdir(dir)) != NULL) {
        if (ptr->d_type == 8) { // file
            count++;
        }
    }
    closedir(dir);
    if (num != count)
        return false;
    for (int i = 0; i < count; ++ i)
        AppendFile(std::to_string(i).c_str(), name);
    return true;
}

//把src文件拼接在aim文件后
bool FILEUTIL::AppendFile(const char* src, const char* aim)
{
    FILE* aimfd = fopen(aim, "a");
    FILE* srcfd = fopen(src, "r");
    if (!aimfd || !srcfd)
        return false;

    int res = 0;
    do {
        res = fread(buf, PIECE_LEN, 1, srcfd);
        fwrite(buf, res, 1, aimfd);
    } while (res == PIECE_LEN);
    return true;
}