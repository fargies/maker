/*
** Copyright (C) 2012 Fargier Sylvain <fargier.sylvain@free.fr>
**
** This software is provided 'as-is', without any express or implied
** warranty.  In no event will the authors be held liable for any damages
** arising from the use of this software.
**
** Permission is granted to anyone to use this software for any purpose,
** including commercial applications, and to alter it and redistribute it
** freely, subject to the following restrictions:
**
** 1. The origin of this software must not be misrepresented; you must not
**    claim that you wrote the original software. If you use this software
**    in a product, an acknowledgment in the product documentation would be
**    appreciated but is not required.
** 2. Altered source versions must be plainly marked as such, and must not be
**    misrepresented as being the original software.
** 3. This notice may not be removed or altered from any source distribution.
**
** main.c
**
**        Created on: Jan 26, 2012
**   Original Author: fargie_s
**
*/

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <poll.h>

#define MAKER_CMD "MAKER_CMD"
#define LOG_FILE ".maker.log"
#define DEFAULT_CMD "/usr/bin/make"
#define SEM_NAME "/maker"

#define BUFFER_SIZE 2048

extern char **environ;

#define mk_error(fmt, args...) \
    fprintf(stderr, "[maker]: " fmt "\n", ##args)

#define mk_info(fmt, args...) \
    fprintf(stderr, "[maker]: " fmt "\n", ##args)

static int launch_cmd(int argc, char *argv[], int log)
{
    sem_t *sem = sem_open(SEM_NAME, O_CREAT, 0644, 0);
    if (sem == SEM_FAILED)
    {
        mk_error("Sem open failed: %s", strerror(errno));
        return 1;
    }

    pid_t pid = fork();
    if (!pid)
    {
        mk_info("Launching command");
        close(log);

        if (daemon(1, 0) != 0)
        {
            mk_error("Daemon failed: %s", strerror(errno));
            exit(1);
        }

        log = open(LOG_FILE, O_WRONLY);
        dup2(log, 1);
        dup2(log, 2);
        close(log);

        struct flock f;
        memset(&f, 0, sizeof (struct flock));
        f.l_whence = SEEK_SET;
        f.l_type = F_WRLCK;

        fcntl(1, F_SETLKW, &f);

        sem_post(sem);
        sem_close(sem);

        char *cmd = getenv(MAKER_CMD);
        if (cmd)
            argv[0] = cmd;
        else
            argv[0] = DEFAULT_CMD;
        execve(argv[0], argv, environ);
        mk_error("Failed to launch command (%s): %s",
                argv[0], strerror(errno));
        exit(1);
    }
    else
    {
        struct flock f;
        memset(&f, 0, sizeof (struct flock));
        f.l_whence = SEEK_SET;
        f.l_type = F_UNLCK;
        fcntl(log, F_SETLK, &f);
        sem_wait(sem);
        sem_close(sem);
        sem_unlink(SEM_NAME);

        int status;
        waitpid(pid, &status, 0);
    }
    return 0;
}

int file_watch_init()
{
    int ino = inotify_init();

    if (ino < 0)
    {
        mk_error("Failed to initialize inotify: %s", strerror(errno));
        return ino;
    }
    else if (inotify_add_watch(ino, LOG_FILE, IN_MODIFY | IN_CLOSE_WRITE) < 0)
    {
        mk_error("Failed to add watch: %s", strerror(errno));
        close(ino);
        return -1;
    }
    return ino;
}

void file_watch_close(int ino)
{
    close(ino);
}

int file_wait_event(int ino, char *buffer, char *write_closed)
{
    struct inotify_event *evt = (struct inotify_event *) buffer;
    int ret = read(ino, buffer, BUFFER_SIZE);

    if (ret < 0)
    {
        mk_error("Wait aborted: %s", strerror(errno));
        return 1;
    }

    while (ret > sizeof(struct inotify_event))
    {
        if (evt->mask & IN_CLOSE_WRITE && write_closed != NULL)
            *write_closed = 1;
        ret -= sizeof(struct inotify_event) + evt->len;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    struct stat st;

    int log = open(LOG_FILE, O_RDONLY | O_CREAT | O_NONBLOCK, 0644);
    if (log < 0 || (fstat(log, &st) != 0))
    {
        mk_error("Failed to open log: %s", strerror(errno));
        return 1;
    }

    struct flock f;
    memset(&f, 0, sizeof (struct flock));
    f.l_type = F_RDLCK;
    f.l_whence = SEEK_SET;

    if ((st.st_size == 0) && (fcntl(log, F_SETLK, &f) == 0))
    {
        if (launch_cmd(argc, argv, log) != 0)
        {
            mk_error("Failed to launch command");
            return 1;
        }
    }

    char *buffer = malloc(BUFFER_SIZE);
    int ino = file_watch_init(LOG_FILE);

    if (ino < 0)
        return 1;

    f.l_type = F_RDLCK;
    char write_closed = 0;
    do
    {
        ssize_t readed;

        if (fcntl(log, F_SETLK, &f) == 0)
            write_closed = 1;

        while ((readed = read(log, buffer, BUFFER_SIZE)) > 0)
        {
            write(1, buffer, readed);
            if (readed < BUFFER_SIZE)
                break;
        }

        if (write_closed != 0)
        {
            mk_info("Command finished");
            close(log);
            unlink(LOG_FILE);
            break;
        }
    }
    while (file_wait_event(ino, buffer, &write_closed) == 0);

    file_watch_close(ino);
    free(buffer);
    return 0;
}

