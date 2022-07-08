/*
 * Copyright (c) 2022 Golioth, Inc.
 * Copyright (c) 2022 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <stdio.h>
#include <stdlib.h>
#include <shell/shell.h>
#include <sys/printk.h>
#include <init.h>
#include <ctype.h>

#include <settings/settings.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_credentials_shell);

static uint8_t raw_buf[4096] = {0};
static int raw_len = 0;
static int raw_buf_offset = 0;
static int json_output = false;
uint8_t name[64] = {0};

static void bypass_cb(const struct shell *shell, uint8_t *data, size_t len)
{
	int err;

	for (int i = 0; i < len; i++)
	{

		// Skip if we get a newline or carriage return
		if (data[i] == '\n' || data[i] == '\r')
			continue;

		raw_buf[raw_buf_offset] = data[i];
		raw_buf_offset += 1;

		/* Disable bypass once we get the byte count as expected. */
		if (raw_len == raw_buf_offset)
		{

			static uint8_t raw_bin_buf[4096] = {0};
			size_t raw_bin_len = hex2bin(raw_buf, raw_len, raw_bin_buf, sizeof(raw_bin_buf));

			/* Convert from hex string to binary */
			if (raw_bin_len == 0)
			{
				LOG_WRN("Invalid hex to bin conversion.");
			}
			else
			{

				LOG_INF("Writing %i bytes to %s", raw_bin_len, log_strdup(name));
#ifdef CONFIG_SETTINGS_RUNTIME
				err = settings_runtime_set(name, raw_bin_buf, raw_bin_len);
				if (err)
				{
					if (json_output)
					{
						shell_fprintf(shell, SHELL_VT100_COLOR_RED,
							      "{\"status\": \"failed\", "
							      "\"msg\": \"Failed to set runtime setting: %s\"}\n",
							      name);
					}
					else
					{
						shell_fprintf(shell, SHELL_ERROR,
							      "Failed to set runtime setting: %s\n", name);
					}
				}
#endif

				err = settings_save_one(name, raw_bin_buf, raw_bin_len);
				if (err)
				{
					if (json_output)
					{
						shell_fprintf(shell, SHELL_VT100_COLOR_RED,
							      "{\"status\": \"failed\", "
							      "\"msg\": \"failed to save setting %s\"}\n",
							      name);
					}
					else
					{
						shell_fprintf(shell, SHELL_ERROR,
							      "Failed to save setting %s\n", name);
					}
				}

				if (json_output)
				{
					shell_fprintf(shell, SHELL_VT100_COLOR_GREEN,
						      "{\"status\": \"success\", \"msg\": \"setting %s\"}\n",
						      name);
				}
				else
				{
					shell_fprintf(shell, SHELL_NORMAL,
						      "Setting %s saved\n", name);
				}
			}

			/* Reset variables */
			json_output = false;
			raw_buf_offset = 0;
			raw_len = 0;
			memset(raw_buf, 0, sizeof(raw_buf));
			memset(name, 0, sizeof(name));

			shell_set_bypass(shell, NULL);

			break;
		}
	}
}

static int cmd_credentials_set(const struct shell *shell, size_t argc, char *argv[])
{

	if (argc < 4)
	{
		shell_fprintf(shell, SHELL_WARNING,
			      "Wrong number of arguments\n");
		shell_help(shell);
		return -ENOEXEC;
	}

	json_output = false;
	if (argc >= 5)
	{
		if (strcmp(argv[4], "--json") == 0)
		{
			json_output = true;
		}
	}

	char *tag = argv[1];
	char *type = argv[2];
	raw_len = atoi(argv[3]);

	snprintf(name, sizeof(name), "pyrinas/cred/%s/%s", tag, type);

	shell_fprintf(shell, SHELL_NORMAL, "Setting %s with %i bytes\n", name, raw_len);

	/* Bypass */
	shell_set_bypass(shell, bypass_cb);

	return 0;
}

static int cmd_credentials_delete(const struct shell *shell, size_t argc,
				  char *argv[])
{

	uint8_t name[64] = {0};
	bool json_output;
	int err;

	const char *tag = argv[1];

	snprintf(name, sizeof(name), "pyrinas/cred/%s", tag);

	json_output = false;
	if (argc >= 3)
	{
		if (strcmp(argv[2], "--json") == 0)
		{
			json_output = true;
		}
	}

	err = settings_delete(tag);
	if (err < 0)
	{
		if (json_output)
		{
			shell_fprintf(shell, SHELL_VT100_COLOR_RED,
				      "{\"status\": \"failed\", \"msg\": \"setting not found\"}\n");
		}
		else
		{
			shell_error(shell, "Setting not found");
		}
	}
	else
	{

		if (json_output)
		{
			shell_fprintf(shell, SHELL_VT100_COLOR_GREEN,
				      "{\"status\": \"success\"}\n");
		}
		else
		{
			shell_fprintf(shell, SHELL_NORMAL,
				      "Setting %s deleted\n", name);
		}
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(credentials_commands,
			       SHELL_CMD_ARG(set, NULL,
					     "set a TLS credential\n"
					     "usage:\n"
					     "$ credentials set <tag> <type> <len>\n"
					     "see enum tls_credential_type for type list"
					     "example:\n"
					     "$ credentials set 1235 0 115\n",
					     cmd_credentials_set,
					     3, 1),
			       SHELL_CMD_ARG(delete, NULL,
					     "delete a TLS credential\n"
					     "usage:\n"
					     "$ credentials delete <tag>\n"
					     "example:\n"
					     "$ credentials delete 1235\n",
					     cmd_credentials_delete, 2, 1),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(credentials, &credentials_commands, "Saving credentials", NULL);
