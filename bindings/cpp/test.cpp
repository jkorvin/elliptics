/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
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
 */

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include <fstream>

#include "elliptics/cppdef.h"

using namespace zbr;

static void test_log_raw(elliptics_log *l, uint32_t mask, const char *format, ...)
{
	va_list args;
	char buf[1024];
	int buflen = sizeof(buf);

	if (!(l->get_log_mask() & mask))
		return;

	va_start(args, format);
	vsnprintf(buf, buflen, format, args);
	buf[buflen-1] = '\0';
	l->log(mask, buf);
	va_end(args);
}

class elliptics_callback_io : public elliptics_callback {
	public:
		elliptics_callback_io(elliptics_log *l) { log = l; };
		virtual ~elliptics_callback_io() {};

		virtual int		callback(void);

	private:
		elliptics_log		*log;
};

int elliptics_callback_io::callback(void)
{
	int err;
	struct dnet_io_attr *io;
	void *data;

	if (is_trans_destroyed(state, cmd, attr)) {
		err = -EINVAL;
		goto err_out_exit;
	}

	if (cmd->status || !cmd->size) {
		err = cmd->status;
		goto err_out_exit;
	}

	if (cmd->size <= sizeof(struct dnet_attr) + sizeof(struct dnet_io_attr)) {
		test_log_raw(log, DNET_LOG_ERROR, "%s: read completion error: wrong size: "
				"cmd_size: %llu, must be more than %zu.\n",
				dnet_dump_id(&cmd->id), (unsigned long long)cmd->size,
				sizeof(struct dnet_attr) + sizeof(struct dnet_io_attr));
		err = -EINVAL;
		goto err_out_exit;
	}

	if (!attr) {
		test_log_raw(log, DNET_LOG_ERROR, "%s: no attributes but command size is not null.\n",
				dnet_dump_id(&cmd->id));
		err = -EINVAL;
		goto err_out_exit;
	}

	io = (struct dnet_io_attr *)(attr + 1);
	data = io + 1;

	dnet_convert_io_attr(io);
	err = 0;

	test_log_raw(log, DNET_LOG_INFO, "%s: io completion: offset: %llu, size: %llu.\n",
			dnet_dump_id(&cmd->id), (unsigned long long)io->offset, (unsigned long long)io->size);

err_out_exit:
	if (!cmd || !(cmd->flags & DNET_FLAGS_MORE))
		test_log_raw(log, DNET_LOG_INFO, "%s: io completed: %d.\n", cmd ? dnet_dump_id(&cmd->id) : "nil", err);
	return err;
}

int main()
{
	int g[] = {1, 2, 3};
	std::vector<int> groups(g, g+ARRAY_SIZE(g));

	try {
		elliptics_log_file log("/dev/stderr", DNET_LOG_ERROR | DNET_LOG_DATA);

		elliptics_node n(log);
		n.add_groups(groups);

		int ports[] = {1025, 1026};
		int added = 0;

		for (int i = 0; i < (int)ARRAY_SIZE(ports); ++i) {
			try {
				n.add_remote("localhost", ports[i], AF_INET);
				added++;
			} catch (...) {
			}
		}

		if (!added)
			throw std::runtime_error("Could not add remote nodes, exiting");

		std::string lobj = "2.xml";
		try {
			std::string lret = n.lookup(lobj);

			struct dnet_addr *addr = (struct dnet_addr *)lret.data();
			struct dnet_cmd *cmd = (struct dnet_cmd *)(addr + 1);
			struct dnet_attr *attr = (struct dnet_attr *)(cmd + 1);
			struct dnet_addr_attr *a = (struct dnet_addr_attr *)(attr + 1);

			dnet_convert_addr_attr(a);
			std::cout << lobj << ": lives on addr: " << dnet_server_convert_dnet_addr(&a->addr);

			if (attr->size > sizeof(struct dnet_addr_attr)) {
				struct dnet_file_info *info = (struct dnet_file_info *)(a + 1);

				dnet_convert_file_info(info);
				std::cout << ": mode: " << std::oct << info->mode;
				std::cout << ", offset: " << info->offset;
				std::cout << ", size: " << info->size;
				std::cout << ", file: " << (char *)(info + 1);
			}
			std::cout << std::endl;
		} catch (const std::exception &e) {
			std::cerr << lobj << ": LOOKUP failed" << std::endl;
		}

		n.stat_log();

		std::string key = "some-key-1";

		std::string data1 = "some-data-in-column-2";
		std::string data2 = "some-data-in-column-3";

		n.write_data_wait(key, data1, 0, 0, 0, 2);
		n.write_data_wait(key, data2, 0, 0, 0, 3);

		/*
		 * metadata write will fail, since it will try to checksum data (column 0),
		 * but we didn't write into that column.
		 */
#if 0
		std::cout << "Writing metadata" << std::endl;

		struct dnet_id id;
		n.transform(key, id);
		id.group_id = 0;
		id.type = 0;

		struct timespec ts = {0, 0};
		n.write_metadata(id, key, groups, ts);
#endif
		std::cout << "read-column-2: " << key << " : " << n.read_data_wait(key, 0, 0, 0, 0, 2) << std::endl;
		std::cout << "read-column-3: " << key << " : " << n.read_data_wait(key, 0, 0, 0, 0, 3) << std::endl;
	} catch (const std::exception &e) {
		std::cerr << "Error occured : " << e.what() << std::endl;
	} catch (int err) {
		std::cerr << "Error : " << err << std::endl;
	}
}
