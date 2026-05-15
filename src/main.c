#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <grp.h>
#include <unistd.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <syslog.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define MAX_JOBS 1024

typedef struct
{
  uint64_t min; 
  uint32_t hour; 
  uint32_t mday; 
  uint16_t mon; 
  uint8_t wday;
  bool mday_restricted; 
  bool wday_restricted;
  char user[64]; 
  char cmd[256]; 
  time_t next_run;
} CronJob;

static CronJob g_jobs[MAX_JOBS];
static int g_job_count = 0;
static volatile sig_atomic_t g_reload = 0;
static volatile sig_atomic_t g_shutdown = 0;

static ssize_t
strscpy (char *dest, const char *src, size_t count)
{
  size_t res = 0;
  if (count == 0)
    return -E2BIG;
  while (res < count)
    {
      dest[res] = src[res];
      if (dest[res] == '\0')
        return res;
      res++;
    }
  dest[count - 1] = '\0';
  return -E2BIG;
}

static void
handle_sigchld (int sig)
{ 
  int saved_errno = errno; 
  (void) sig; 
  while (waitpid (-1, NULL, WNOHANG) > 0); 
  errno = saved_errno; 
}

static void
handle_sighup (int sig)
{ 
  (void) sig; 
  g_reload = 1; 
}

static void
handle_sigterm (int sig)
{ 
  (void) sig; 
  g_shutdown = 1; 
}

static uint64_t
parse_field (char *str, int min_val, int max_val)
{
  uint64_t mask = 0;
  char *tok, *saveptr;
  
  for (tok = strtok_r (str, ",", &saveptr); tok; tok = strtok_r (NULL, ",", &saveptr))
    {
      int step = 1;
      char *slash = strchr (tok, '/'), *dash;
      
      if (slash)
        {
          *slash = '\0';
          step = atoi (slash + 1);
          if (step <= 0)
            step = 1;
        }
      if (strcmp (tok, "*") == 0)
        {
          for (int i = min_val; i <= max_val; i += step)
            mask |= (1ULL << i);
          continue;
        }
      dash = strchr (tok, '-');
      if (dash)
        {
          *dash = '\0';
          int start = atoi (tok), end = atoi (dash + 1);
          if (start >= min_val && start <= max_val && end >= min_val && end <= max_val && start <= end)
            for (int i = start; i <= end; i += step)
              mask |= (1ULL << i);
          continue;
        }
      int val = atoi (tok);
      if (val >= min_val && val <= max_val)
        mask |= (1ULL << val);
    }
  return mask;
}

static inline int
next_set_bit (uint64_t mask, int start, int max)
{
  uint64_t shifted;
  if (start > max)
    return -1;
  shifted = mask >> start;
  if (shifted == 0)
    return -1;
  return start + __builtin_ctzll (shifted);
}

static time_t
calculate_next_run (CronJob *job, time_t now)
{
  struct tm t;
  time_t check = now + 60;
  check -= (check % 60);

  while (1)
    {
      if (!localtime_r (&check, &t))
        return 0;

      if (UNLIKELY (t.tm_year > 800))
        return 0;

      if (!(job->mon & (1ULL << (t.tm_mon + 1))))
        {
          int n = next_set_bit (job->mon, t.tm_mon + 1, 12);
          if (n < 0)
            {
              t.tm_year++;
              t.tm_mon = 0;
            }
          else
            {
              t.tm_mon = n - 1;
            }
          t.tm_mday = 1; t.tm_hour = 0; t.tm_min = 0; t.tm_isdst = -1;
          check = mktime (&t);
          continue;
        }

      bool mday_match = (job->mday & (1ULL << t.tm_mday)) != 0;
      bool wday_match = (job->wday & (1ULL << t.tm_wday)) != 0;
      if (t.tm_wday == 0 && (job->wday & (1ULL << 7)))
        wday_match = true;

      bool day_match = (job->mday_restricted && job->wday_restricted) ?
                       (mday_match || wday_match) : (mday_match && wday_match);

      if (!day_match)
        {
          t.tm_mday++;
          t.tm_hour = 0; t.tm_min = 0; t.tm_isdst = -1;
          check = mktime (&t);
          continue;
        }

      if (!(job->hour & (1ULL << t.tm_hour)))
        {
          int n = next_set_bit (job->hour, t.tm_hour, 23);
          if (n < 0)
            {
              t.tm_mday++;
              t.tm_hour = 0;
            }
          else
            {
              t.tm_hour = n;
            }
          t.tm_min = 0; t.tm_isdst = -1;
          check = mktime (&t);
          continue;
        }

      if (!(job->min & (1ULL << t.tm_min)))
        {
          int n = next_set_bit (job->min, t.tm_min, 59);
          if (n < 0)
            {
              t.tm_hour++;
              t.tm_min = 0;
            }
          else
            {
              t.tm_min = n;
            }
          t.tm_isdst = -1;
          check = mktime (&t);
          continue;
        }

      return check;
    }
}

static void
load_crontab (void)
{
  FILE *f;
  char line[512];
  time_t now = time (NULL);

  g_job_count = 0;
  if (!(f = fopen ("/etc/crontab", "re")))
    { 
      syslog (LOG_WARNING, "Cannot open /etc/crontab: %m"); 
      return; 
    }

  while (fgets (line, sizeof (line), f) && g_job_count < MAX_JOBS)
    {
      char *p = line;
      char min_s[128], hr_s[128], mday_s[128], mon_s[128], wday_s[128], user_s[128];
      int bytes_read = 0;

      while (isspace ((unsigned char)*p))
        p++;
      if (*p == '#' || *p == '\0')
        continue;

      if (sscanf (p, "%127s %127s %127s %127s %127s %127s %n", min_s, hr_s, mday_s, mon_s, wday_s, user_s, &bytes_read) == 6)
        {
          char *cmd = p + bytes_read;
          while (isspace ((unsigned char)*cmd))
            cmd++;
          if (*cmd == '\0')
            continue;

          CronJob *j = &g_jobs[g_job_count];
          j->min = parse_field (min_s, 0, 59);
          j->hour = parse_field (hr_s, 0, 23);
          j->mday = parse_field (mday_s, 1, 31);
          j->mon = parse_field (mon_s, 1, 12);
          j->wday = parse_field (wday_s, 0, 7);

          if (!j->min || !j->hour || !j->mday || !j->mon || !j->wday)
            continue;

          j->mday_restricted = (strcmp (mday_s, "*") != 0);
          j->wday_restricted = (strcmp (wday_s, "*") != 0);

          if (strscpy (j->user, user_s, sizeof (j->user)) < 0)
            {
              syslog (LOG_ERR, "Job dropped: Username exceeds buffer bounds");
              continue;
            }
            
          if (strscpy (j->cmd, cmd, sizeof (j->cmd)) < 0)
            {
              syslog (LOG_ERR, "Job dropped: Command string exceeds bounds");
              continue;
            }
          j->cmd[strcspn (j->cmd, "\r\n")] = '\0';

          if ((j->next_run = calculate_next_run (j, now)) != 0)
            g_job_count++;
        }
    }
  fclose (f);
  syslog (LOG_INFO, "Loaded %d jobs from /etc/crontab", g_job_count);
}

static void
setup_signals (void)
{
  struct sigaction sa;
  memset (&sa, 0, sizeof (sa));
  sa.sa_handler = handle_sigchld;
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction (SIGCHLD, &sa, NULL);
  
  sa.sa_flags = 0;
  sa.sa_handler = handle_sighup;
  sigaction (SIGHUP, &sa, NULL);
  
  sa.sa_handler = handle_sigterm;
  sigaction (SIGTERM, &sa, NULL);
  sigaction (SIGINT, &sa, NULL);
}

__attribute__((hot)) int
main (void)
{
  openlog ("bh-crond", LOG_PID | LOG_CONS, LOG_DAEMON);
  syslog (LOG_INFO, "Starting Minimal Tickless Cron Daemon...");

  setup_signals ();
  load_crontab ();

  while (!g_shutdown)
    {
      time_t now;
      time_t next_wakeup;

      if (UNLIKELY (g_reload))
        {
          load_crontab ();
          g_reload = 0;
        }

      now = time (NULL);
      next_wakeup = now + (86400 * 365);

      for (int i = 0; i < g_job_count; i++)
        {
          if (UNLIKELY (g_jobs[i].next_run <= now))
            {
              pid_t pid = fork ();
              if (pid == 0)
                {
                  int fd = open ("/dev/null", O_RDWR | O_CLOEXEC);
                  struct passwd *pw;

                  if (fd >= 0)
                    {
                      dup2 (fd, STDIN_FILENO);
                      dup2 (fd, STDOUT_FILENO);
                      dup2 (fd, STDERR_FILENO);
                      if (fd > STDERR_FILENO)
                        close (fd);
                    }

                  pw = getpwnam (g_jobs[i].user);
                  if (!pw)
                    {
                      syslog (LOG_ERR, "Invalid user '%s'", g_jobs[i].user);
                      exit (EXIT_FAILURE);
                    }

                  if (initgroups (pw->pw_name, pw->pw_gid) != 0 || setregid (pw->pw_gid, pw->pw_gid) != 0 || setreuid (pw->pw_uid, pw->pw_uid) != 0)
                    exit (EXIT_FAILURE);

                  setenv ("USER", pw->pw_name, 1);
                  setenv ("LOGNAME", pw->pw_name, 1);
                  setenv ("HOME", pw->pw_dir, 1);
                  setenv ("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 0);
                  setenv ("SHELL", "/bin/sh", 1);

                  execl ("/bin/sh", "sh", "-c", g_jobs[i].cmd, NULL);
                  exit (127);
                }
              g_jobs[i].next_run = calculate_next_run (&g_jobs[i], now);
            }
          if (g_jobs[i].next_run < next_wakeup)
            next_wakeup = g_jobs[i].next_run;
        }

      if (LIKELY (next_wakeup > now && !g_shutdown && !g_reload))
        {
          struct timespec ts;
          ts.tv_sec = next_wakeup;
          ts.tv_nsec = 0;
          clock_nanosleep (CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL);
        }
    }
  closelog ();
  return 0;
}