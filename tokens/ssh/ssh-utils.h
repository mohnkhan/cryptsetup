/*
 * ssh plugin utilities
 *
 * Copyright (C) 2016-2021 Milan Broz <gmazyland@gmail.com>
 * Copyright (C) 2020-2021 Vojtech Trefny
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <libcryptsetup.h>

int sshplugin_download_password(struct crypt_device *cd, ssh_session ssh,
	const char *path, char **password, size_t *password_len);
ssh_session sshplugin_session_init(struct crypt_device *cd, const char *host, const char *user);
int sshplugin_public_key_auth(struct crypt_device *cd, ssh_session ssh, const ssh_key pkey);