#include <stdio.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int read_cpu_totals(unsigned long long *idle, unsigned long long *total) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    // 第一行格式通常是：
    // cpu  user nice system idle iowait irq softirq steal guest guest_nice
    char buf[256];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    unsigned long long user=0, nice=0, system=0, idle_v=0, iowait=0, irq=0, softirq=0, steal=0;
    // 只解析常见前 8 项就够用了
    int n = sscanf(buf, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &system, &idle_v, &iowait, &irq, &softirq, &steal);
    if (n < 4) return -1;

    *idle = idle_v + iowait;
    *total = user + nice + system + idle_v + iowait + irq + softirq + steal;
    return 0;
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


static int cpu_usage_percent(cpu_stat_t *prev, double *cpu_percent) {
    unsigned long long idle=0, total=0;
    if (read_cpu_totals(&idle, &total) != 0) return -1;

    if (!prev->valid) {
        // 第一次调用，没有上一次数据，无法算“这一段时间”的比例
        prev->idle = idle;
        prev->total = total;
        prev->valid = 1;
        *cpu_percent = 0.0;
        return 0;
    }

    unsigned long long idle_delta = idle - prev->idle;
    unsigned long long total_delta = total - prev->total;

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
    cpu_stat_t cpu_prev = {0};
    int opt;
    
    int json_mode = 0;

	// 过滤掉 --json，避免 getopt 报错
    for (int i = 1; i < argc; ) {
        if (strcmp(argv[i], "--json") == 0) {
            json_mode = 1;

            // 删除 argv[i]：把后面的参数整体左移一格
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;          // 参数个数减少
            // 注意：这里不 i++，因为新移过来的 argv[i] 还没检查
        } else {
            i++;
        }
    }
    const char *disk_path = "/";

    opterr = 0;
    while ((opt = getopt(argc, argv, "i:n:p:hj")) != -1) {
        switch (opt) {
            case 'i':
                interval_sec = atoi(optarg);
                if (interval_sec < 0) interval_sec = 0;
                break;
            case 'n':
                count = atol(optarg);
                if (count < 0) count = 0;
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

    while (1) {
        double load1 = 0.0;
        double mem_used = 0.0;
	double cpu = 0.0;
	double disk = 0.0;
	double up_sec = 0.0;
	
	if (read_uptime_seconds(&up_sec) != 0) {
    	printf("read uptime failed\n");
    	return 1;
	}
	
	if (read_disk_used_percent(disk_path, &disk) != 0) {
    	printf("read disk failed for path:%s\n",disk_path);
    	return 1;
	}
	char up_str[32];
	format_uptime(up_sec, up_str, sizeof(up_str));

        if (read_load1(&load1) != 0) {
            printf("read load1 failed\n");
            return 1;
        }

        if (read_mem_used_percent(&mem_used) != 0) {
            printf("read meminfo failed\n");
            return 1;
        }

	if (cpu_usage_percent(&cpu_prev, &cpu) != 0) {
   	 printf("read cpu failed\n");
   	 return 1;
	}

        char ts[32];
        make_timestamp(ts, sizeof(ts));

	if (!json_mode) {
    	printf("%s  load1=%.2f  mem=%.1f%%  cpu=%.1f%%  disk=%.1f%%  up=%s\n",
          	 ts, load1, mem_used, cpu, disk, up_str);
	}
	else
	{
    	printf("{\"ts\":\"%s\",\"load1\":%.2f,\"mem\":%.1f,\"cpu\":%.1f,\"disk\":%.1f,\"up\":\"%s\"}\n",
           	ts, load1, mem_used, cpu, disk, up_str);
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

