/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2003-2004  Theodore A. Roth  <troth@openavr.org>
 * Copyright (C) 2006 Joerg Wunsch <j@uriah.heep.sax.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* $Id$ */

/*
 * Posix serial interface for avrdude.
 */

#if !defined(WIN32NATIVE)

#include "ac_cfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "avrdude.h"
#include "libavrdude.h"

#ifndef SERIAL_REC_DEF_TIMEOUT_MS
 #define SERIAL_REC_DEF_TIMEOUT_MS 5000
#endif
long serial_recv_timeout =  SERIAL_REC_DEF_TIMEOUT_MS; /* ms */

#ifdef serial_settimeout
static int ser_settimeout (long timeout)
{
   if (timeout < 1000)
   {  
	    serial_recv_timeout = SERIAL_REC_DEF_TIMEOUT_MS;
   }
   else
   {
      serial_recv_timeout = timeout;
   }
   return(serial_recv_timeout);
}
#endif		

struct baud_mapping {
  long baud;
  speed_t speed;
};

/* There are a lot more baud rates we could handle, but what's the point? */

static struct baud_mapping baud_lookup_table[] = {
    {1200, B1200},
    {2400, B2400},
    {4800, B4800},
    {9600, B9600},
    {19200, B19200},
    {38400, B38400},
#ifdef B57600
    {57600, B57600},
#endif
#ifdef B115200
    {115200, B115200},
#endif
#ifdef B230400
    {230400, B230400},
#endif
    /*#ifdef B460800
    {460800, B460800},
#endif
#ifdef B921600
    {921600, B921600},
#endif */
    {0, 0} /* Terminator. */
};

static struct termios original_termios;
static int saved_original_termios;

static speed_t serial_baud_lookup(long baud)
{
  struct baud_mapping *map = baud_lookup_table;

  while (map->baud) {
    if (map->baud == baud)
      return map->speed;
    map++;
  }

  /*
   * If a non-standard BAUD rate is used, issue
   * a warning (if we are verbose) and return the raw rate
   */
  avrdude_message(MSG_NOTICE, "%s: serial_baud_lookup(): Using non-standard baud rate: %ld",
              progname, baud);

  return baud;
}

static int ser_setspeed(union filedescriptor *fd, long baud)
{
  int rc;
  struct termios termios;
  speed_t speed = serial_baud_lookup (baud);
  
  if (!isatty(fd->ifd))
    return -ENOTTY;

#ifdef serial_settimeout
  ser_settimeout(-1);
#endif
  
  /*
   * initialize terminal modes
   */
  rc = tcgetattr(fd->ifd, &termios);
  if (rc < 0) {
    avrdude_message(MSG_INFO, "%s: ser_setspeed(): tcgetattr() failed",
            progname);
    return -errno;
  }

  /*
   * copy termios for ser_close if we haven't already
   */
  if (! saved_original_termios++) {
    original_termios = termios;
  }

  termios.c_iflag = IGNBRK;
  termios.c_oflag = 0;
  termios.c_lflag = 0;
  termios.c_cflag = (CS8 | CREAD | CLOCAL);
  termios.c_cc[VMIN]  = 1;
  termios.c_cc[VTIME] = 0;

  cfsetospeed(&termios, speed);
  cfsetispeed(&termios, speed);

  rc = tcsetattr(fd->ifd, TCSANOW, &termios);
  if (rc < 0) {
    avrdude_message(MSG_INFO, "%s: ser_setspeed(): tcsetattr() failed\n",
            progname);
    return -errno;
  }

  /*
   * Everything is now set up for a local line without modem control
   * or flow control, so clear O_NONBLOCK again.
   */
  rc = fcntl(fd->ifd, F_GETFL, 0);
  if (rc != -1)
    fcntl(fd->ifd, F_SETFL, rc & ~O_NONBLOCK);

  return 0;
}

/*
 * Given a port description of the form <host>:<port>, open a TCP
 * connection to the specified destination, which is assumed to be a
 * terminal/console server with serial parameters configured
 * appropriately (e. g. 115200-8-N-1 for a STK500.)
 */
static int
net_open(const char *port, union filedescriptor *fdp)
{
#ifdef HAVE_GETADDRINFO
  char *hp, *hstr, *pstr;
  int s, fd, ret = -1;
  struct addrinfo hints;
  struct addrinfo *result, *rp;

  if ((hstr = hp = strdup(port)) == NULL) {
    avrdude_message(MSG_INFO, "%s: net_open(): Out of memory!\n",
	    progname);
    return -1;
  }

  /*
   * As numeric IPv6 addresses use colons as separators, we need to
   * look for the last colon here, which separates the port number or
   * service name from the host or IP address.
   */
  if (((pstr = strrchr(hstr, ':')) == NULL) || (pstr == hstr)) {
    avrdude_message(MSG_INFO, "%s: net_open(): Mangled host:port string \"%s\"\n",
                    progname, hstr);
    goto error;
  }

  /*
   * Remove brackets from the host part, if present.
   */
  if (*hstr == '[' && *(pstr-1) == ']') {
    hstr++;
    *(pstr-1) = '\0';
  }

  /*
   * Terminate the host section of the description.
   */
  *pstr++ = '\0';

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  s = getaddrinfo(hstr, pstr, &hints, &result);

  if (s != 0) {
    avrdude_message(MSG_INFO,
                    "%s: net_open(): Cannot resolve "
                    "host=\"%s\", port=\"%s\": %s\n",
                    progname, hstr, pstr, gai_strerror(s));
    goto error;
  }
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1) {
      /* This one failed, loop over */
      continue;
    }
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) != -1) {
      /* Success, we are connected */
      break;
    }
    close(fd);
  }
  if (rp == NULL) {
    avrdude_message(MSG_INFO, "%s: net_open(): Cannot connect: %s\n",
                    progname, strerror(errno));
  }
  else {
    fdp->ifd = fd;
    ret = 0;
  }
  freeaddrinfo(result);

error:
  free(hp);
  return ret;
#else
  avrdude_message(MSG_INFO,
                  "%s: Networking is not supported on your platform.\n"
                  "If you need it, please open a bug report.\n",
                  progname);
  return -1;
#endif /* HAVE_GETADDRINFO */
}


static int ser_set_dtr_rts(union filedescriptor *fdp, int is_on)
{
  unsigned int ctl;
  int r;

  r = ioctl(fdp->ifd, TIOCMGET, &ctl);
  if (r < 0) {
    perror("ioctl(\"TIOCMGET\")");
    return -1;
  }

  if (is_on) {
    /* Set DTR and RTS */
    ctl |= (TIOCM_DTR | TIOCM_RTS);
  }
  else {
    /* Clear DTR and RTS */
    ctl &= ~(TIOCM_DTR | TIOCM_RTS);
  }

  r = ioctl(fdp->ifd, TIOCMSET, &ctl);
  if (r < 0) {
    perror("ioctl(\"TIOCMSET\")");
    return -1;
  }

  return 0;
}

static int ser_open(char * port, union pinfo pinfo, union filedescriptor *fdp)
{
  int rc;
  int fd;

  /*
   * If the port is of the form "net:<host>:<port>", then
   * handle it as a TCP connection to a terminal server.
   */
  if (strncmp(port, "net:", strlen("net:")) == 0) {
    return net_open(port + strlen("net:"), fdp);
  }

  /*
   * open the serial port
   */
  fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    avrdude_message(MSG_INFO, "%s: ser_open(): can't open device \"%s\": %s\n",
                    progname, port, strerror(errno));
    return -1;
  }

  fdp->ifd = fd;

  /*
   * set serial line attributes
   */
  rc = ser_setspeed(fdp, pinfo.baud);
  if (rc) {
    avrdude_message(MSG_INFO, "%s: ser_open(): can't set attributes for device \"%s\": %s\n",
                    progname, port, strerror(-rc));
    close(fd);
    return -1;
  }
  return 0;
}


static void ser_close(union filedescriptor *fd)
{
  /*
   * restore original termios settings from ser_open
   */
  if (saved_original_termios) {
    int rc = tcsetattr(fd->ifd, TCSANOW | TCSADRAIN, &original_termios);
    if (rc) {
      avrdude_message(MSG_INFO, "%s: ser_close(): can't reset attributes for device: %s\n",
                      progname, strerror(errno));
    }
    saved_original_termios = 0;
  }

  close(fd->ifd);
}


static int ser_send(union filedescriptor *fd, const unsigned char * buf, size_t buflen)
{
  int rc;
  const unsigned char * p = buf;
  size_t len = buflen;

  if (!len)
    return 0;

  if (verbose > 3)
  {
    avrdude_message(MSG_TRACE, "%s: Send: ", progname);

      while (buflen) {
        unsigned char c = *buf;
        if (isprint(c)) {
          avrdude_message(MSG_TRACE, "%c ", c);
        }
        else {
          avrdude_message(MSG_TRACE, ". ");
        }
        avrdude_message(MSG_TRACE, "[%02x] ", c);

        buf++;
        buflen--;
      }

      avrdude_message(MSG_TRACE, "\n");
  }

  while (len) {
    rc = write(fd->ifd, p, (len > 1024) ? 1024 : len);
    if (rc < 0) {
      avrdude_message(MSG_INFO, "%s: ser_send(): write error: %s\n",
                      progname, strerror(errno));
      return -1;
    }
    p += rc;
    len -= rc;
  }

  return 0;
}


static int ser_recv(union filedescriptor *fd, unsigned char * buf, size_t buflen)
{
  struct timeval timeout, to2;
  fd_set rfds;
  int nfds;
  int rc;
  unsigned char * p = buf;
  size_t len = 0;

  timeout.tv_sec  = serial_recv_timeout / 1000L;
  timeout.tv_usec = (serial_recv_timeout % 1000L) * 1000;
  to2 = timeout;

  while (len < buflen) {
  reselect:
    FD_ZERO(&rfds);
    FD_SET(fd->ifd, &rfds);

    nfds = select(fd->ifd + 1, &rfds, NULL, NULL, &to2);
    if (nfds == 0) {
      avrdude_message(MSG_NOTICE2, "%s: ser_recv(): programmer is not responding\n",
                      progname);
      return -1;
    }
    else if (nfds == -1) {
      if (errno == EINTR || errno == EAGAIN) {
	avrdude_message(MSG_INFO, "%s: ser_recv(): programmer is not responding,reselecting\n",
                        progname);
        goto reselect;
      }
      else {
        avrdude_message(MSG_INFO, "%s: ser_recv(): select(): %s\n",
                        progname, strerror(errno));
        return -1;
      }
    }

    rc = read(fd->ifd, p, (buflen - len > 1024) ? 1024 : buflen - len);
    if (rc < 0) {
      avrdude_message(MSG_INFO, "%s: ser_recv(): read error: %s\n",
                      progname, strerror(errno));
      return -1;
    }
    p += rc;
    len += rc;
  }

  p = buf;

  if (verbose > 3)
  {
    avrdude_message(MSG_TRACE, "%s: Recv: ", progname);

    while (len)
    {
      unsigned char c = *p;
      if (isprint(c)) {
        avrdude_message(MSG_TRACE, "%c ", c);
      } else {
        avrdude_message(MSG_TRACE, ". ");
      }
      avrdude_message(MSG_TRACE, "[%02x] ", c);

      p++;
      len--;
    }
    avrdude_message(MSG_TRACE, "\n");
  }

  return 0;
}


static int ser_drain(union filedescriptor *fd, int display)
{
  struct timeval timeout;
  fd_set rfds;
  int nfds;
  int rc;
  unsigned char buf;

  timeout.tv_sec = 0;
  timeout.tv_usec = 250000;

  if (display) {
    avrdude_message(MSG_INFO, "drain>");
  }

  while (1) {
    FD_ZERO(&rfds);
    FD_SET(fd->ifd, &rfds);

  reselect:
    nfds = select(fd->ifd + 1, &rfds, NULL, NULL, &timeout);
    if (nfds == 0) {
      if (display) {
        avrdude_message(MSG_INFO, "<drain\n");
      }
      
      break;
    }
    else if (nfds == -1) {
      if (errno == EINTR) {
        goto reselect;
      }
      else {
        avrdude_message(MSG_INFO, "%s: ser_drain(): select(): %s\n",
                        progname, strerror(errno));
        return -1;
      }
    }

    rc = read(fd->ifd, &buf, 1);
    if (rc < 0) {
      avrdude_message(MSG_INFO, "%s: ser_drain(): read error: %s\n",
                      progname, strerror(errno));
      return -1;
    }
    if (display) {
      avrdude_message(MSG_INFO, "%02x ", buf);
    }
  }

  return 0;
}

struct serial_device serial_serdev =
{
  .open = ser_open,
  .setspeed = ser_setspeed,
  .close = ser_close,
  .send = ser_send,
  .recv = ser_recv,
  .drain = ser_drain,
  .set_dtr_rts = ser_set_dtr_rts,
  .flags = SERDEV_FL_CANSETSPEED,
};

struct serial_device *serdev = &serial_serdev;

#endif /* WIN32NATIVE */
