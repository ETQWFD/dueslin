/*
 * DUESLIN Burner - C 语言版
 * 用法: ./dueslin-burner <iso文件名> [输出目录]
 * 编译: gcc -o dueslin-burner dueslin-burner.c -lcurl -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/stat.h>

#define REPO "ETQWFD/dueslin"
#define API_URL "https://api.github.com/repos/" REPO "/releases/latest"
/* 国内加速镜像 */
#define MIRROR1 "https://ghproxy.com/"
#define MIRROR2 "https://mirror.ghproxy.com/"

struct Memory { char *data; size_t len; };

static size_t write_cb(void *p, size_t s, size_t n, void *u) {
    size_t total = s*n;
    struct Memory *m = (struct Memory *)u;
    m->data = (char *)realloc(m->data, m->len+total+1);
    memcpy(m->data+m->len, p, total);
    m->len += total;
    m->data[m->len] = 0;
    return total;
}

/* 从 JSON 中提取指定 asset 的下载 URL（简单解析） */
char *find_url(const char *json, const char *name) {
    char pat[512];
    snprintf(pat, sizeof(pat), "\"browser_download_url\"");
    /* 简化: 找 name 后面的 url */
    char *p = (char *)strstr(json, name);
    if (!p) return NULL;
    p = strstr(p, "\"browser_download_url\"");
    if (!p) return NULL;
    p = strchr(p, '"'); p = strchr(p+1, '"'); p++;
    char *end = strchr(p, '"');
    if (!end) return NULL;
    int len = end - p;
    char *url = (char *)malloc(len+1);
    memcpy(url, p, len); url[len] = 0;
    return url;
}

static size_t write_file(void *p, size_t s, size_t n, void *fp) {
    return fwrite(p, s, n, (FILE*)fp);
}

static int progress_cb(void *p, double dlt, double dlnow, double ult, double ulnow) {
    (void)p;(void)ult;(void)ulnow;
    if (dlt > 0) {
        int pct = (int)(dlnow*100/dlt);
        printf("\r  下载进度: %d%%  (%.1f/%.1f MB)",
               pct, dlnow/1048576, dlt/1048576);
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("DUESLIN Burner v1.0 (C)\n");
        printf("用法: %s <iso名称> [输出目录]\n", argv[0]);
        printf("可选: DUESLIN-1.0.iso, DUESLIN-Server-1.0.iso, DUESLIN-Mini-1.0.iso\n");
        return 1;
    }
    const char *iso = argv[1];
    const char *outdir = argc > 2 ? argv[2] : ".";

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *c = curl_easy_init();
    if (!c) { fprintf(stderr, "curl init failed\n"); return 1; }

    /* 1. 查询最新 release */
    printf("[1/3] 查询 GitHub Release...\n");
    struct Memory m = {0};
    curl_easy_setopt(c, CURLOPT_URL, API_URL);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "DUESLIN-Burner");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30);
    CURLcode r = curl_easy_perform(c);
    if (r != CURLE_OK) { fprintf(stderr, "GitHub API 失败: %s\n", curl_easy_strerror(r)); return 1; }

    char *url = find_url(m.data, iso);
    if (!url) { fprintf(stderr, "未找到 %s\n", iso); free(m.data); return 1; }

    /* 尝试国内镜像加速 */
    char fast[2048];
    snprintf(fast, sizeof(fast), "%s%s", MIRROR1, url);

    /* 2. 下载 */
    printf("[2/3] 下载 %s\n", iso);
    char out[2048];
    snprintf(out, sizeof(out), "%s/%s", outdir, iso);
    FILE *fp = fopen(out, "wb");
    if (!fp) { perror("fopen"); return 1; }

    curl_easy_setopt(c, CURLOPT_URL, fast);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_PROGRESSFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 0L);
    r = curl_easy_perform(c);
    fclose(fp);
    printf("\n");

    if (r != CURLE_OK) {
        /* 镜像失败，尝试直连 */
        printf("镜像失败，尝试直连...\n");
        fp = fopen(out, "wb");
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, fp);
        r = curl_easy_perform(c);
        fclose(fp);
    }

    /* 3. 完成 */
    if (r == CURLE_OK) {
        printf("[3/3] 完成! 文件: %s\n", out);
        printf("请用 dd 或 BalenaEtcher 刻录到 U 盘:\n");
        printf("  sudo dd if=%s of=/dev/sdX bs=4M status=progress\n", out);
    } else {
        fprintf(stderr, "下载失败: %s\n", curl_easy_strerror(r));
    }

    free(url); free(m.data);
    curl_easy_cleanup(c);
    curl_global_cleanup();
    return r == CURLE_OK ? 0 : 1;
}
