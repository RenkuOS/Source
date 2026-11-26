/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Checks that a configuration-changing ioctl by an unprivileged user is
	refused before it is dispatched to a driver. On an unfixed stack the
	request reaches the (nonexistent) interface and fails with ENODEV
	instead of EPERM. Run as a normal user, not root.
*/


#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <syslog.h>
#include <unistd.h>


int
main()
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		printf("net_ioctl_permissions_test: socket: %s\n", strerror(errno));
		return 2;
	}

	uid_t saved = geteuid();
	if (saved == 0 && seteuid(65534) != 0) {
		puts("net_ioctl_permissions_test: cannot drop root privileges");
		close(fd);
		return 2;
	}

	struct ifreq request;
	memset(&request, 0, sizeof(request));
	strlcpy(request.ifr_name, "no_such_if0", IF_NAMESIZE);
	request.ifr_mtu = 1500;

	int result = ioctl(fd, SIOCSIFMTU, &request);
	int error = errno;

	if (saved == 0)
		seteuid(saved);
	close(fd);

	printf("net_ioctl_permissions_test: euid %u, ioctl returned %d, errno %d "
		"(%s)\n", (unsigned)saved, result, error, strerror(error));

	bool denied = error == EPERM || error == EACCES;
	const char* verdict = denied
		? "ok, the configuration change was refused"
		: "FAILED, the request was dispatched (errno is not EPERM)";
	printf("RESULT: %s\n", verdict);
	syslog(LOG_INFO, "net_ioctl_permissions_test: RESULT: %s", verdict);
	return denied ? 0 : 1;
}
