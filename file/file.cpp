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
    printf("in merge file\n");
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
    bool rt = true;
    for (int i = 0; i < count; ++i)
        rt &= AppendFile(std::to_string(i).c_str(), name);
    return rt;
}

//把src文件拼接在aim文件后
bool FILEUTIL::AppendFile(const char* src, const char* aim)
{
    string srcpath = std::string(aim) + "/" + std::string(src);
    string aimpath = std::string(aim) + "/" + std::string(aim);
    FILE* aimfd = fopen(aimpath.c_str(), "ab");
    FILE* srcfd = fopen(srcpath.c_str(), "rb");
    printf("aim:%p src:%p\n", aimfd, srcfd);
    if (!aimfd || !srcfd)
        return false;

    int res = fread(buf, PIECE_LEN, 1, srcfd);
    printf("read:%d\n", res);
    fwrite(buf, res, 1, aimfd);
    fclose(aimfd);
    fclose(srcfd);
    return true;
}