#include <stdio.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>


typedef struct {
    unsigned long long idle;
    unsigned long long total;
    int valid;
} cpu_stat_t;

static int cpu_usage_percent(cpu_stat_t *prev, double *cpu_percent);

static void make_timestamp(char *buf, size_t buflen) {
    time_t now = time(NULL);           // 取当前时间（秒）
    struct tm tm_now;

    localtime_r(&now, &tm_now);        // 转成“本地时间”的年月日时分秒
    strftime(buf, buflen, "%Y-%m-%d %H:%M:%S", &tm_now); // 格式化成字符串
}

static int read_load1(double *load1) {
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return -1;
    int ret = fscanf(fp, "%lf", load1);
    fclose(fp);
    return (ret == 1) ? 0 : -1;
}

static int read_uptime_seconds(double *uptime_sec) {
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) return -1;

    int ret = fscanf(fp, "%lf", uptime_sec);  // 只读第一列
    fclose(fp);

    return (ret == 1) ? 0 : -1;
}


static int read_mem_used_percent(double *used_percent) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;

    char line[256];
    long mem_total_kb = -1;
    long mem_avail_kb = -1;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line + 9, "%ld", &mem_total_kb);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line + 13, "%ld", &mem_avail_kb);
        }

        if (mem_total_kb > 0 && mem_avail_kb >= 0) {
            break;
        }
    }

    fclose(fp);

    if (mem_total_kb <= 0 || mem_avail_kb < 0) return -1;

    long used_kb = mem_total_kb - mem_avail_kb;
    *used_percent = (double)used_kb * 100.0 / (double)mem_total_kb;
    return 0;
}

static int read_cpu_totals_by_index(int cpu_index,unsigned long long *idle,unsigned long long *total)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1; 

    char target[16];
    if (cpu_index < 0)
        snprintf(target, sizeof(target), "cpu");
    else
        snprintf(target, sizeof(target), "cpu%d", cpu_index);

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // 只匹配开头是 cpu/cpu0/cpu1...
        if (strncmp(line, target, strlen(target)) != 0) continue;
        // 避免 cpu0 匹配到 cpu01 这种情况（加一个边界判断）
        if (line[strlen(target)] != ' ' && line[strlen(target)] != '\t') continue;

        unsigned long long user=0, nice=0, system=0, idle_v=0, iowait=0, irq=0, softirq=0, steal=0;
        // 用 %*s 跳过 cpu/cpu0/cpu1 字符串，兼容总cpu行和每核行
        int n = sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &system, &idle_v, &iowait, &irq, &softirq, &steal);

        if (n < 4) { // 至少要有 user nice system idle
            fclose(fp);
            return -1;
        }

        *idle  = idle_v + (n >= 5 ? iowait  : 0);
        *total = user + nice + system + idle_v
               + (n >= 5 ? iowait  : 0)
               + (n >= 6 ? irq     : 0)
               + (n >= 7 ? softirq : 0)
               + (n >= 8 ? steal   : 0);

        fclose(fp);
        return 0;
    }

    fclose(fp);
    return -1; // 没找到对应 cpu 行
}

static int read_cpu_totals(unsigned long long *idle, unsigned long long *total) {
    return read_cpu_totals_by_index(-1, idle, total);
}



static int read_disk_used_percent(const char *path, double *used_percent) {
    struct statvfs s;
    if (statvfs(path, &s) != 0) {
        return -1;
    }

    unsigned long long total = (unsigned long long)s.f_blocks * (unsigned long long)s.f_frsize;
    unsigned long long avail = (unsigned long long)s.f_bavail * (unsigned long long)s.f_frsize;

    if (total == 0) return -1;

    unsigned long long used = total - avail;
    *used_percent = (double)used * 100.0 / (double)total;
    return 0;
}


static int cpu_usage_percent_by_index(int cpu_index, cpu_stat_t *prev, double *cpu_percent) {
    unsigned long long idle=0, total=0;
    if (read_cpu_totals_by_index(cpu_index,&idle, &total) != 0) return -1;

    if (!prev->valid) {
        // 第一次调用，没有上一次数据，无法算“这一段时间”的比例
        //所以做参数初始化
        prev->idle = idle;
        prev->total = total;
        prev->valid = 1;
        *cpu_percent = 0.0;
        return 0;
    }
    // 计算增量
    unsigned long long idle_delta = idle - prev->idle;
    unsigned long long total_delta = total - prev->total;
    //立刻更新 prev，留给下一次使用
    prev->idle = idle;
    prev->total = total;
    if (total_delta == 0) {
        *cpu_percent = 0.0;
        return 0;
    }
    double usage = 100.0 * (1.0 - (double)idle_delta / (double)total_delta);
    if (usage < 0.0) usage = 0.0;
    if (usage > 100.0) usage = 100.0;

    *cpu_percent = usage;
    return 0;
}

static int cpu_usage_percent(cpu_stat_t *prev, double *cpu_percent)
{
    return cpu_usage_percent_by_index(-1, prev, cpu_percent);
}



static void usage(const char *prog) {
    printf("Usage: %s [-i interval_sec] [-n count]\n", prog);
    printf("  -i interval_sec  refresh interval in seconds (default: 0, print once)\n");
    printf("  -n count         number of lines to print (default: 1 if no -i, otherwise infinite)\n");
    printf("  -p path          filesystem path for disk usage (default: /)\n");
    printf("  -j  --json             output in JSON format\n");
}

static void format_uptime(double uptime_sec, char *buf, size_t buflen) {
    long long sec = (long long)(uptime_sec + 0.5); // 四舍五入到整数秒

    long long days = sec / 86400;
    sec %= 86400;
    long long hours = sec / 3600;
    sec %= 3600;
    long long mins = sec / 60;

    if (days > 0) {
        snprintf(buf, buflen, "%lldd%02lldh%02lldm", days, hours, mins);
    } else if (hours > 0) {
        snprintf(buf, buflen, "%lldh%02lldm", hours, mins);
    } else {
        snprintf(buf, buflen, "%lldm", mins);
    }
}


int main(int argc, char **argv) {
    int interval_sec = 0;     // 默认不循环
    long count = -1;          // -1 表示“无限”（只有 interval_sec>0 时才有意义）
    int n_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    cpu_stat_t *cpu_prev_arr = calloc(n_cpus, sizeof(cpu_stat_t));
    cpu_stat_t cpu_prev_total = {0};
    int percpu_mode = 0;    
    int opt;
    int json_mode = 0;
    const char *disk_path = "/";
    opterr = 0;

    static struct option long_opts[] = 
    {
        {"interval", required_argument, 0, 'i'},
        {"count",    required_argument, 0, 'n'},
        {"path",     required_argument, 0, 'p'},
        {"json",     no_argument,       0, 'j'},
        {"help",     no_argument,       0, 'h'},
        {"percpu",  no_argument,        0, 'C'},
        {0, 0, 0, 0}
    };

    int long_index = 0;
    while ((opt = getopt_long(argc, argv, "i:n:p:hj", long_opts, &long_index)) != -1) {
        switch (opt) {
            case 'i':
                interval_sec = atoi(optarg);
                if (interval_sec < 0) interval_sec = 0;
                break;
            case 'n':
                count = atol(optarg);
                if (count < 0) count = 0;
                break;
            case 'C':
                percpu_mode = 1;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return 1;
	    case 'j':
		json_mode = 1;
		break;
	    case '?':
   		usage(argv[0]);
    		return 1;
	    case 'p':
                disk_path = optarg;
                break;

        }
    }

    // 默认次数规则：
    // - 如果 interval_sec == 0：打印一次就退出（count 默认=1）
    // - 如果 interval_sec  > 0：默认无限打印（count 默认=-1）
    if (interval_sec == 0 && count == -1) {
        count = 1;
    }

    long printed = 0;
    if (n_cpus < 1) n_cpus = 1;
    if (!cpu_prev_arr) {
        perror("calloc cpu_prev_arr");
        return 1;
    }


    while (1) {
        double load1 = 0.0, mem_used = 0.0, cpu = 0.0, disk = 0.0, up_sec = 0.0;
        int ok_load = 0, ok_mem = 0, ok_cpu = 0, ok_disk = 0, ok_up = 0;

        ok_up   = (read_uptime_seconds(&up_sec) == 0);
        ok_disk = (read_disk_used_percent(disk_path, &disk) == 0);
        ok_load = (read_load1(&load1) == 0);
        ok_mem  = (read_mem_used_percent(&mem_used) == 0);
        ok_cpu  = (cpu_usage_percent(&cpu_prev_total, &cpu) == 0);
	
        char ts[32];
        make_timestamp(ts, sizeof(ts));
	
	    char up_str[32] = "N/A";
            if (ok_up) {
                format_uptime(up_sec, up_str, sizeof(up_str));
            }

        //判断json模式
	    if (!json_mode) {
            printf("%s  ", ts);

            if (ok_load) printf("load1=%.2f  ", load1);
            else         printf("load1=N/A  ");

            if (percpu_mode) {
            for (int i = 0; i < n_cpus; i++) {
            double c = 0.0;
            int ok = (cpu_usage_percent_by_index(i, &cpu_prev_arr[i], &c) == 0);
            if (ok) printf("cpu%d=%.2f%%  ", i, c);
            else    printf("cpu%d=N/A  ", i);
            }
        }

            if (ok_mem)  printf("mem=%.1f%%  ", mem_used);
            else         printf("mem=N/A  ");

            if (ok_cpu)  printf("cpu=%.2f%%  ", cpu);
            else         printf("cpu=N/A  ");

            if (ok_disk) printf("disk=%.1f%%  ", disk);
            else         printf("disk=N/A  ");

            printf("up=%s\n", up_str);
        }
	    else
	    {
            // JSON: 失败字段输出 null（更标准）
            printf("{\"ts\":\"%s\",", ts);

            if (ok_load) printf("\"load1\":%.2f,", load1);
            else         printf("\"load1\":null,");

            if (ok_mem)  printf("\"mem\":%.1f,", mem_used);
            else         printf("\"mem\":null,");

            if (ok_cpu)  printf("\"cpu\":%.2f,", cpu);
            else         printf("\"cpu\":null,");

            if (percpu_mode) {
            for (int i = 0; i < n_cpus; i++) {
                double c = 0.0;
                int ok = (cpu_usage_percent_by_index(i, &cpu_prev_arr[i], &c) == 0);
                if (ok) printf("\"cpu%d\":%.2f,", i, c);
                else    printf("\"cpu%d\":null,", i);
            }
        }

            if (ok_disk) printf("\"disk\":%.1f,", disk);
            else         printf("\"disk\":null,");

            printf("\"up\":\"%s\"}\n", up_str);
        }
	
        fflush(stdout);
        printed++;

        // 退出条件
        if (interval_sec == 0) break;                  // 不循环模式：打印一次就走
        if (count != -1 && printed >= count) break;    // 指定了次数：够了就走

        sleep((unsigned int)interval_sec);
    }

    return 0;
}

