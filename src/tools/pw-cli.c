/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>

#include <spa/utils/result.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>

#include <pipewire/impl.h>

#include <extensions/session-manager.h>

static const char WHITESPACE[] = " \t";

struct remote_data;

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct spa_list remotes;
	struct remote_data *current;

	struct pw_map vars;
};

struct global {
	struct remote_data *rd;
	uint32_t id;
	uint32_t permissions;
	uint32_t version;
	char *type;
	struct pw_proxy *proxy;
	bool info_pending;
	struct pw_properties *properties;
};

struct remote_data {
	struct spa_list link;
	struct data *data;

	char *name;
	uint32_t id;

	int prompt_pending;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook proxy_core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_map globals;
};

struct proxy_data;

typedef void (*info_func_t) (struct proxy_data *pd);

struct proxy_data {
	struct remote_data *rd;
	struct global *global;
	struct pw_proxy *proxy;
        void *info;
	info_func_t info_func;
        pw_destroy_t destroy;
        struct spa_hook proxy_listener;
        struct spa_hook object_listener;
};

struct command {
	const char *name;
	const char *alias;
	const char *description;
	bool (*func) (struct data *data, const char *cmd, char *args, char **error);
};

static int pw_split_ip(char *str, const char *delimiter, int max_tokens, char *tokens[])
{
	const char *state = NULL;
	char *s;
	size_t len;
	int n = 0;

        s = (char *)pw_split_walk(str, delimiter, &len, &state);
        while (s && n + 1 < max_tokens) {
		s[len] = '\0';
		tokens[n++] = s;
                s = (char*)pw_split_walk(str, delimiter, &len, &state);
        }
        if (s) {
		tokens[n++] = s;
        }
        return n;
}

static struct pw_properties *parse_props(char *str)
{
	const char *state = NULL;
	char *s, *p[3];
	size_t len, n;
	struct pw_properties *props = NULL;

	while (true) {
		s = (char *)pw_split_walk(str, WHITESPACE, &len, &state);
		if (s == NULL)
			break;

		s[len] = '\0';
		n = pw_split_ip(s, "=", 2, p);
		if (n == 2) {
			if (props == NULL)
				props = pw_properties_new(p[0], p[1], NULL);
			else
				pw_properties_set(props, p[0], p[1]);
		}
	}
	return props;
}

static void print_properties(struct spa_dict *props, char mark, bool header)
{
	const struct spa_dict_item *item;

	if (header)
		fprintf(stdout, "%c\tproperties:\n", mark);
	if (props == NULL || props->n_items == 0) {
		if (header)
			fprintf(stdout, "\t\tnone\n");
		return;
	}

	spa_dict_for_each(item, props) {
		fprintf(stdout, "%c\t\t%s = \"%s\"\n", mark, item->key, item->value);
	}
}

static void print_params(struct spa_param_info *params, uint32_t n_params, char mark, bool header)
{
	uint32_t i;

	if (header)
		fprintf(stdout, "%c\tparams: (%u)\n", mark, n_params);
	if (params == NULL || n_params == 0) {
		if (header)
			fprintf(stdout, "\t\tnone\n");
		return;
	}
	for (i = 0; i < n_params; i++) {
		const struct spa_type_info *type_info = spa_type_param;

		fprintf(stdout, "%c\t  %d (%s) %c%c\n", mark, params[i].id,
			spa_debug_type_find_name(type_info, params[i].id),
			params[i].flags & SPA_PARAM_INFO_READ ? 'r' : '-',
			params[i].flags & SPA_PARAM_INFO_WRITE ? 'w' : '-');
	}
}

static bool do_not_implemented(struct data *data, const char *cmd, char *args, char **error)
{
        *error = spa_aprintf("Command \"%s\" not yet implemented", cmd);
	return false;
}

static bool do_help(struct data *data, const char *cmd, char *args, char **error);
static bool do_load_module(struct data *data, const char *cmd, char *args, char **error);
static bool do_list_objects(struct data *data, const char *cmd, char *args, char **error);
static bool do_connect(struct data *data, const char *cmd, char *args, char **error);
static bool do_disconnect(struct data *data, const char *cmd, char *args, char **error);
static bool do_list_remotes(struct data *data, const char *cmd, char *args, char **error);
static bool do_switch_remote(struct data *data, const char *cmd, char *args, char **error);
static bool do_info(struct data *data, const char *cmd, char *args, char **error);
static bool do_create_device(struct data *data, const char *cmd, char *args, char **error);
static bool do_create_node(struct data *data, const char *cmd, char *args, char **error);
static bool do_destroy(struct data *data, const char *cmd, char *args, char **error);
static bool do_create_link(struct data *data, const char *cmd, char *args, char **error);
static bool do_export_node(struct data *data, const char *cmd, char *args, char **error);
static bool do_enum_params(struct data *data, const char *cmd, char *args, char **error);
static bool do_permissions(struct data *data, const char *cmd, char *args, char **error);
static bool do_get_permissions(struct data *data, const char *cmd, char *args, char **error);

static struct command command_list[] = {
	{ "help", "h", "Show this help", do_help },
	{ "load-module", "lm", "Load a module. <module-name> [<module-arguments>]", do_load_module },
	{ "unload-module", "um", "Unload a module. <module-var>", do_not_implemented },
	{ "connect", "con", "Connect to a remote. [<remote-name>]", do_connect },
	{ "disconnect", "dis", "Disconnect from a remote. [<remote-var>]", do_disconnect },
	{ "list-remotes", "lr", "List connected remotes.", do_list_remotes },
	{ "switch-remote", "sr", "Switch between current remotes. [<remote-var>]", do_switch_remote },
	{ "list-objects", "ls", "List objects or current remote. [<interface>]", do_list_objects },
	{ "info", "i", "Get info about an object. <object-id>|all", do_info },
	{ "create-device", "cd", "Create a device from a factory. <factory-name> [<properties>]", do_create_device },
	{ "create-node", "cn", "Create a node from a factory. <factory-name> [<properties>]", do_create_node },
	{ "destroy", "d", "Destroy a global object. <object-id>", do_destroy },
	{ "create-link", "cl", "Create a link between nodes. <node-id> <port-id> <node-id> <port-id> [<properties>]", do_create_link },
	{ "export-node", "en", "Export a local node to the current remote. <node-id> [remote-var]", do_export_node },
	{ "enum-params", "e", "Enumerate params of an object <object-id> [<param-id-name>]", do_enum_params },
	{ "permissions", "sp", "Set permissions for a client <client-id> <object> <permission>", do_permissions },
	{ "get-permissions", "gp", "Get permissions of a client <client-id>", do_get_permissions },
};

static bool do_help(struct data *data, const char *cmd, char *args, char **error)
{
	size_t i;

	fprintf(stdout, "Available commands:\n");
	for (i = 0; i < SPA_N_ELEMENTS(command_list); i++) {
		fprintf(stdout, "\t%-20.20s\t%s\n", command_list[i].name, command_list[i].description);
	}
	return true;
}

static bool do_load_module(struct data *data, const char *cmd, char *args, char **error)
{
	struct pw_impl_module *module;
	char *a[2];
	int n;
	uint32_t id;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		*error = spa_aprintf("%s <module-name> [<module-arguments>]", cmd);
		return false;
	}

	module = pw_context_load_module(data->context, a[0], n == 2 ? a[1] : NULL, NULL);
	if (module == NULL) {
		*error = spa_aprintf("Could not load module");
		return false;
	}

	id = pw_map_insert_new(&data->vars, module);
	fprintf(stdout, "%d = @module:%d\n", id, pw_global_get_id(pw_impl_module_get_global(module)));

	return true;
}

static void on_core_info(void *_data, const struct pw_core_info *info)
{
	struct remote_data *rd = _data;
	free(rd->name);
	rd->name = info->name ? strdup(info->name) : NULL;
	fprintf(stdout, "remote %d is named '%s'\n", rd->id, rd->name);
}

static void show_prompt(struct remote_data *rd)
{
	fprintf(stdout, "%s>>", rd->name);
	fflush(stdout);
}

static void on_core_done(void *_data, uint32_t id, int seq)
{
	struct remote_data *rd = _data;

	if (seq == rd->prompt_pending)
		show_prompt(rd);
}

static int print_global(void *obj, void *data)
{
	struct global *global = obj;
	const char *filter = data;

	if (global == NULL)
		return 0;

	if (filter && !strstr(global->type, filter))
		return 0;

	fprintf(stdout, "\tid %d, type %s/%d\n", global->id,
					global->type, global->version);
	if (global->properties)
		print_properties(&global->properties->dict, ' ', false);

	return 0;
}


static bool bind_global(struct remote_data *rd, struct global *global, char **error);

static void registry_event_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct remote_data *rd = data;
	struct global *global;
	size_t size;
	char *error;
	bool ret;

	global = calloc(1, sizeof(struct global));
	global->rd = rd;
	global->id = id;
	global->permissions = permissions;
	global->type = strdup(type);
	global->version = version;
	global->properties = props ? pw_properties_new_dict(props) : NULL;

	fprintf(stdout, "remote %d added global: ", rd->id);
	print_global(global, NULL);

	size = pw_map_get_size(&rd->globals);
	while (id > size)
		pw_map_insert_at(&rd->globals, size++, NULL);
	pw_map_insert_at(&rd->globals, id, global);

	/* immediately bind the object always */
	ret = bind_global(rd, global, &error);
	if (!ret) {
		fprintf(stdout, "Error: \"%s\"\n", error);
		free(error);
	}
}

static int destroy_global(void *obj, void *data)
{
	struct global *global = obj;

	if (global == NULL)
		return 0;

	pw_map_remove(&global->rd->globals, global->id);
	if (global->properties)
		pw_properties_free(global->properties);
	free(global->type);
	free(global);
	return 0;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct remote_data *rd = data;
	struct global *global;

	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		fprintf(stdout, "remote %d removed unknown global %d\n", rd->id, id);
		return;
	}

	fprintf(stdout, "remote %d removed global: ", rd->id);
	print_global(global, NULL);
	destroy_global(global, rd);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct remote_data *rd = _data;
	struct data *data = rd->data;

	pw_log_error("remote %p: error id:%u seq:%d res:%d (%s): %s", rd,
			id, seq, res, spa_strerror(res), message);

	if (id == 0) {
		pw_main_loop_quit(data->loop);
	}
}

static const struct pw_core_events remote_core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = on_core_info,
	.done = on_core_done,
	.error = on_core_error,
};

static void on_core_destroy(void *_data)
{
	struct remote_data *rd = _data;
	struct data *data = rd->data;

	spa_list_remove(&rd->link);

	pw_map_remove(&data->vars, rd->id);
	pw_map_for_each(&rd->globals, destroy_global, rd);

	if (data->current == rd)
		data->current = NULL;
	free(rd->name);
}

static const struct pw_proxy_events proxy_core_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = on_core_destroy,
};

static bool do_connect(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n;
	struct pw_properties *props = NULL;
	struct pw_core *core;
	struct remote_data *rd;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n == 1) {
		props = pw_properties_new(PW_KEY_REMOTE_NAME, a[0], NULL);
	}
	core = pw_context_connect(data->context, props, sizeof(struct remote_data));
	if (core == NULL) {
		*error = spa_aprintf("failed to connect: %m");
		return false;
	}

	rd = pw_proxy_get_user_data((struct pw_proxy*)core);
	rd->core = core;
	rd->data = data;
	pw_map_init(&rd->globals, 64, 16);
	rd->id = pw_map_insert_new(&data->vars, rd);
	spa_list_append(&data->remotes, &rd->link);

	fprintf(stdout, "%d = @remote:%p\n", rd->id, rd->core);
	data->current = rd;

	pw_core_add_listener(rd->core,
				   &rd->core_listener,
				   &remote_core_events, rd);
	pw_proxy_add_listener((struct pw_proxy*)rd->core,
			&rd->proxy_core_listener,
			&proxy_core_events, rd);
	rd->registry = pw_core_get_registry(rd->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(rd->registry,
				       &rd->registry_listener,
				       &registry_events, rd);
	rd->prompt_pending = pw_core_sync(rd->core, 0, 0);

	return true;
}

static bool do_disconnect(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n;
	uint32_t idx;
	struct remote_data *rd = data->current;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n >= 1) {
		idx = atoi(a[0]);
		rd = pw_map_lookup(&data->vars, idx);
		if (rd == NULL)
			goto no_remote;

	}
	pw_core_disconnect(rd->core);

	if (data->current == NULL) {
		if (spa_list_is_empty(&data->remotes)) {
			return true;
		}
		data->current = spa_list_last(&data->remotes, struct remote_data, link);
	}

	return true;

      no_remote:
        *error = spa_aprintf("Remote %d does not exist", idx);
	return false;
}

static bool do_list_remotes(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd;

	spa_list_for_each(rd, &data->remotes, link)
		fprintf(stdout, "\t%d = @remote:%p '%s'\n", rd->id, rd->core, rd->name);

	return true;
}

static bool do_switch_remote(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n, idx = 0;
	struct remote_data *rd;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n == 1)
		idx = atoi(a[0]);

	rd = pw_map_lookup(&data->vars, idx);
	if (rd == NULL)
		goto no_remote;

	spa_list_remove(&rd->link);
	spa_list_append(&data->remotes, &rd->link);
	data->current = rd;

	return true;

      no_remote:
        *error = spa_aprintf("Remote %d does not exist", idx);
	return false;
}

#define MARK_CHANGE(f) ((((info)->change_mask & (f))) ? '*' : ' ')

static void info_global(struct proxy_data *pd)
{
	struct global *global = pd->global;

	if (global == NULL)
		return;

	fprintf(stdout, "\tid: %d\n", global->id);
	fprintf(stdout, "\tpermissions: %c%c%c\n", global->permissions & PW_PERM_R ? 'r' : '-',
					  global->permissions & PW_PERM_W ? 'w' : '-',
					  global->permissions & PW_PERM_X ? 'x' : '-');
	fprintf(stdout, "\ttype: %s/%d\n", global->type, global->version);
}

static void info_core(struct proxy_data *pd)
{
	struct pw_core_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tcookie: %u\n", info->cookie);
	fprintf(stdout, "\tuser-name: \"%s\"\n", info->user_name);
	fprintf(stdout, "\thost-name: \"%s\"\n", info->host_name);
	fprintf(stdout, "\tversion: \"%s\"\n", info->version);
	fprintf(stdout, "\tname: \"%s\"\n", info->name);
	print_properties(info->props, MARK_CHANGE(PW_CORE_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_module(struct proxy_data *pd)
{
	struct pw_module_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tname: \"%s\"\n", info->name);
	fprintf(stdout, "\tfilename: \"%s\"\n", info->filename);
	fprintf(stdout, "\targs: \"%s\"\n", info->args);
	print_properties(info->props, MARK_CHANGE(PW_MODULE_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_node(struct proxy_data *pd)
{
	struct pw_node_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "%c\tinput ports: %u/%u\n", MARK_CHANGE(PW_NODE_CHANGE_MASK_INPUT_PORTS),
			info->n_input_ports, info->max_input_ports);
	fprintf(stdout, "%c\toutput ports: %u/%u\n", MARK_CHANGE(PW_NODE_CHANGE_MASK_OUTPUT_PORTS),
			info->n_output_ports, info->max_output_ports);
	fprintf(stdout, "%c\tstate: \"%s\"", MARK_CHANGE(PW_NODE_CHANGE_MASK_STATE),
			pw_node_state_as_string(info->state));
	if (info->state == PW_NODE_STATE_ERROR && info->error)
		fprintf(stdout, " \"%s\"\n", info->error);
	else
		fprintf(stdout, "\n");
	print_properties(info->props, MARK_CHANGE(PW_NODE_CHANGE_MASK_PROPS), true);
	print_params(info->params, info->n_params, MARK_CHANGE(PW_NODE_CHANGE_MASK_PARAMS), true);
	info->change_mask = 0;
}

static void info_port(struct proxy_data *pd)
{
	struct pw_port_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tdirection: \"%s\"\n", pw_direction_as_string(info->direction));
	print_properties(info->props, MARK_CHANGE(PW_PORT_CHANGE_MASK_PROPS), true);
	print_params(info->params, info->n_params, MARK_CHANGE(PW_PORT_CHANGE_MASK_PARAMS), true);
	info->change_mask = 0;
}

static void info_factory(struct proxy_data *pd)
{
	struct pw_factory_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tname: \"%s\"\n", info->name);
	fprintf(stdout, "\tobject-type: %s/%d\n", info->type, info->version);
	print_properties(info->props, MARK_CHANGE(PW_FACTORY_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_client(struct proxy_data *pd)
{
	struct pw_client_info *info = pd->info;

	info_global(pd);
	print_properties(info->props, MARK_CHANGE(PW_CLIENT_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_link(struct proxy_data *pd)
{
	struct pw_link_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\toutput-node-id: %u\n", info->output_node_id);
	fprintf(stdout, "\toutput-port-id: %u\n", info->output_port_id);
	fprintf(stdout, "\tinput-node-id: %u\n", info->input_node_id);
	fprintf(stdout, "\tinput-port-id: %u\n", info->input_port_id);

	fprintf(stdout, "%c\tstate: \"%s\"", MARK_CHANGE(PW_LINK_CHANGE_MASK_STATE),
			pw_link_state_as_string(info->state));
	if (info->state == PW_LINK_STATE_ERROR && info->error)
		printf(" \"%s\"\n", info->error);
	else
		printf("\n");
	fprintf(stdout, "%c\tformat:\n", MARK_CHANGE(PW_LINK_CHANGE_MASK_FORMAT));
	if (info->format)
		spa_debug_format(2, NULL, info->format);
	else
		fprintf(stdout, "\t\tnone\n");
	print_properties(info->props, MARK_CHANGE(PW_LINK_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_device(struct proxy_data *pd)
{
	struct pw_device_info *info = pd->info;

	info_global(pd);
	print_properties(info->props, MARK_CHANGE(PW_DEVICE_CHANGE_MASK_PROPS), true);
	print_params(info->params, info->n_params, MARK_CHANGE(PW_DEVICE_CHANGE_MASK_PARAMS), true);
	info->change_mask = 0;
}

static void info_session(struct proxy_data *pd)
{
	struct pw_session_info *info = pd->info;

	info_global(pd);
	print_properties(info->props, MARK_CHANGE(0), true);
	print_params(info->params, info->n_params, MARK_CHANGE(1), true);
	info->change_mask = 0;
}

static void info_endpoint(struct proxy_data *pd)
{
	struct pw_endpoint_info *info = pd->info;
	const char *direction;

	info_global(pd);
	fprintf(stdout, "\tname: %s\n", info->name);
	fprintf(stdout, "\tmedia-class: %s\n",  info->media_class);
	switch(info->direction) {
	case PW_DIRECTION_OUTPUT:
		direction = "source";
		break;
	case PW_DIRECTION_INPUT:
		direction = "sink";
		break;
	default:
		direction = "invalid";
		break;
	}
	fprintf(stdout, "\tdirection: %s\n", direction);
	fprintf(stdout, "\tflags: 0x%x\n", info->flags);
	fprintf(stdout, "%c\tstreams: %u\n", MARK_CHANGE(0), info->n_streams);
	fprintf(stdout, "%c\tsession: %u\n", MARK_CHANGE(1), info->session_id);
	print_properties(info->props, MARK_CHANGE(2), true);
	print_params(info->params, info->n_params, MARK_CHANGE(3), true);
	info->change_mask = 0;
}

static void info_endpoint_stream(struct proxy_data *pd)
{
	struct pw_endpoint_stream_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tid: %u\n", info->id);
	fprintf(stdout, "\tendpoint-id: %u\n", info->endpoint_id);
	fprintf(stdout, "\tname: %s\n", info->name);
	print_properties(info->props, MARK_CHANGE(1), true);
	print_params(info->params, info->n_params, MARK_CHANGE(2), true);
	info->change_mask = 0;
}

static void core_event_info(void *object, const struct pw_core_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d core %d changed\n", rd->id, info->id);
	pd->info = pw_core_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_core(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = core_event_info
};


static void module_event_info(void *object, const struct pw_module_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d module %d changed\n", rd->id, info->id);
	pd->info = pw_module_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_module(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.info = module_event_info
};

static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d node %d changed\n", rd->id, info->id);
	pd->info = pw_node_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_node(pd);
		pd->global->info_pending = false;
	}
}

static void event_param(void *object, int seq, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
        struct proxy_data *data = object;
	struct remote_data *rd = data->rd;

	fprintf(stdout, "remote %d object %d param %d index %d\n",
			rd->id, data->global->id, id, index);

	if (spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Format))
		spa_debug_format(2, NULL, param);
	else
		spa_debug_pod(2, NULL, param);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = event_param
};


static void port_event_info(void *object, const struct pw_port_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d port %d changed\n", rd->id, info->id);
	pd->info = pw_port_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_port(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.info = port_event_info,
	.param = event_param
};

static void factory_event_info(void *object, const struct pw_factory_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d factory %d changed\n", rd->id, info->id);
	pd->info = pw_factory_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_factory(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_factory_events factory_events = {
	PW_VERSION_FACTORY_EVENTS,
	.info = factory_event_info
};

static void client_event_info(void *object, const struct pw_client_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d client %d changed\n", rd->id, info->id);
	pd->info = pw_client_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_client(pd);
		pd->global->info_pending = false;
	}
}

static void client_event_permissions(void *object, uint32_t index,
		uint32_t n_permissions, const struct pw_permission *permissions)
{
        struct proxy_data *data = object;
	struct remote_data *rd = data->rd;
	uint32_t i;

	fprintf(stdout, "remote %d node %d index %d\n",
			rd->id, data->global->id, index);

	for (i = 0; i < n_permissions; i++) {
		if (permissions[i].id == PW_ID_ANY)
			fprintf(stdout, "  default:");
		else
			fprintf(stdout, "  %u:", permissions[i].id);
		fprintf(stdout, " %08x\n", permissions[i].permissions);
	}
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_event_info,
	.permissions = client_event_permissions
};

static void link_event_info(void *object, const struct pw_link_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d link %d changed\n", rd->id, info->id);
	pd->info = pw_link_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_link(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = link_event_info
};


static void device_event_info(void *object, const struct pw_device_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d device %d changed\n", rd->id, info->id);
	pd->info = pw_device_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_device(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = event_param
};

static void session_info_free(struct pw_session_info *info)
{
	free(info->params);
	if (info->props)
		pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void session_event_info(void *object,
				const struct pw_session_info *update)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct pw_session_info *info = pd->info;

	if (!info) {
		info = pd->info = calloc(1, sizeof(*info));
		info->id = update->id;
	}
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS) {
		info->n_params = update->n_params;
		free(info->params);
		info->params = malloc(info->n_params * sizeof(struct spa_param_info));
		memcpy(info->params, update->params,
			info->n_params * sizeof(struct spa_param_info));
	}
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS) {
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		info->props =
			(struct spa_dict *) pw_properties_new_dict (update->props);
	}

	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_session(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_session_events session_events = {
	PW_VERSION_SESSION_EVENTS,
	.info = session_event_info,
	.param = event_param
};

static void endpoint_info_free(struct pw_endpoint_info *info)
{
	free(info->name);
	free(info->media_class);
	free(info->params);
	if (info->props)
		pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void endpoint_event_info(void *object,
				const struct pw_endpoint_info *update)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct pw_endpoint_info *info = pd->info;

	if (!info) {
		info = pd->info = calloc(1, sizeof(*info));
		info->id = update->id;
		info->name = update->name ? strdup(update->name) : NULL;
		info->media_class = update->media_class ? strdup(update->media_class) : NULL;
		info->direction = update->direction;
		info->flags = update->flags;
	}
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_STREAMS)
		info->n_streams = update->n_streams;
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_SESSION)
		info->session_id = update->session_id;
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS) {
		info->n_params = update->n_params;
		free(info->params);
		info->params = malloc(info->n_params * sizeof(struct spa_param_info));
		memcpy(info->params, update->params,
			info->n_params * sizeof(struct spa_param_info));
	}
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS) {
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		info->props =
			(struct spa_dict *) pw_properties_new_dict (update->props);
	}

	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_endpoint(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_endpoint_events endpoint_events = {
	PW_VERSION_ENDPOINT_EVENTS,
	.info = endpoint_event_info,
	.param = event_param
};

static void endpoint_stream_info_free(struct pw_endpoint_stream_info *info)
{
	free(info->name);
	free(info->params);
	if (info->props)
		pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void endpoint_stream_event_info(void *object,
				const struct pw_endpoint_stream_info *update)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct pw_endpoint_stream_info *info = pd->info;

	if (!info) {
		info = pd->info = calloc(1, sizeof(*info));
		info->id = update->id;
		info->endpoint_id = update->endpoint_id;
		info->name = update->name ? strdup(update->name) : NULL;
	}
	if (update->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PARAMS) {
		info->n_params = update->n_params;
		free(info->params);
		info->params = malloc(info->n_params * sizeof(struct spa_param_info));
		memcpy(info->params, update->params,
			info->n_params * sizeof(struct spa_param_info));
	}
	if (update->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PROPS) {
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		info->props =
			(struct spa_dict *) pw_properties_new_dict (update->props);
	}

	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_endpoint_stream(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_endpoint_stream_events endpoint_stream_events = {
	PW_VERSION_ENDPOINT_STREAM_EVENTS,
	.info = endpoint_stream_event_info,
	.param = event_param
};

static void
destroy_proxy (void *data)
{
	struct proxy_data *pd = data;

	if (pd->info == NULL)
		return;

	if (pd->global)
		pd->global->proxy = NULL;

	if (pd->destroy)
		pd->destroy(pd->info);
	pd->info = NULL;
}

static const struct pw_proxy_events proxy_events = {
        PW_VERSION_PROXY_EVENTS,
        .destroy = destroy_proxy,
};

static bool do_list_objects(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	pw_map_for_each(&rd->globals, print_global, args);
	return true;
}

static bool bind_global(struct remote_data *rd, struct global *global, char **error)
{
        const void *events;
        uint32_t client_version;
	info_func_t info_func;
        pw_destroy_t destroy;
	struct proxy_data *pd;
	struct pw_proxy *proxy;

	if (strcmp(global->type, PW_TYPE_INTERFACE_Core) == 0) {
		events = &core_events;
		client_version = PW_VERSION_CORE;
		destroy = (pw_destroy_t) pw_core_info_free;
		info_func = info_core;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Module) == 0) {
		events = &module_events;
		client_version = PW_VERSION_MODULE;
		destroy = (pw_destroy_t) pw_module_info_free;
		info_func = info_module;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Device) == 0) {
		events = &device_events;
		client_version = PW_VERSION_DEVICE;
		destroy = (pw_destroy_t) pw_device_info_free;
		info_func = info_device;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Node) == 0) {
		events = &node_events;
		client_version = PW_VERSION_NODE;
		destroy = (pw_destroy_t) pw_node_info_free;
		info_func = info_node;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Port) == 0) {
		events = &port_events;
		client_version = PW_VERSION_PORT;
		destroy = (pw_destroy_t) pw_port_info_free;
		info_func = info_port;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Factory) == 0) {
		events = &factory_events;
		client_version = PW_VERSION_FACTORY;
		destroy = (pw_destroy_t) pw_factory_info_free;
		info_func = info_factory;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Client) == 0) {
		events = &client_events;
		client_version = PW_VERSION_CLIENT;
		destroy = (pw_destroy_t) pw_client_info_free;
		info_func = info_client;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Link) == 0) {
		events = &link_events;
		client_version = PW_VERSION_LINK;
		destroy = (pw_destroy_t) pw_link_info_free;
		info_func = info_link;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Session) == 0) {
		events = &session_events;
		client_version = PW_VERSION_SESSION;
		destroy = (pw_destroy_t) session_info_free;
		info_func = info_session;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Endpoint) == 0) {
		events = &endpoint_events;
		client_version = PW_VERSION_ENDPOINT;
		destroy = (pw_destroy_t) endpoint_info_free;
		info_func = info_endpoint;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream) == 0) {
		events = &endpoint_stream_events;
		client_version = PW_VERSION_ENDPOINT_STREAM;
		destroy = (pw_destroy_t) endpoint_stream_info_free;
		info_func = info_endpoint_stream;
	} else {
		*error = spa_aprintf("unsupported type %s", global->type);
		return false;
	}

	proxy = pw_registry_bind(rd->registry,
				       global->id,
				       global->type,
				       client_version,
				       sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->global = global;
	pd->proxy = proxy;
	pd->info_func = info_func;
	pd->destroy = destroy;
	pw_proxy_add_object_listener(proxy, &pd->object_listener, events, pd);
	pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	global->proxy = proxy;

	return true;
}

static bool do_global_info(struct global *global, char **error)
{
	struct remote_data *rd = global->rd;
	struct proxy_data *pd;

	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
		global->info_pending = true;
	} else {
		pd = pw_proxy_get_user_data(global->proxy);
		if (pd->info_func)
			pd->info_func(pd);
	}
	return true;
}
static int do_global_info_all(void *obj, void *data)
{
	struct global *global = obj;
	char *error;

	if (global == NULL)
		return 0;

	if (!do_global_info(global, &error)) {
		fprintf(stderr, "info: %s\n", error);
		free(error);
	}
	return 0;
}

static bool do_info(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[1];
        int n;
	uint32_t id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		*error = spa_aprintf("%s <object-id>|all", cmd);
		return false;
	}
	if (strcmp(a[0], "all") == 0) {
		pw_map_for_each(&rd->globals, do_global_info_all, NULL);
	}
	else {
		id = atoi(a[0]);
		global = pw_map_lookup(&rd->globals, id);
		if (global == NULL) {
			*error = spa_aprintf("%s: unknown global %d", cmd, id);
			return false;
		}
		return do_global_info(global, error);
	}
	return true;
}

static bool do_create_device(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[2];
	int n;
	uint32_t id;
	struct pw_proxy *proxy;
	struct pw_properties *props = NULL;
	struct proxy_data *pd;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		*error = spa_aprintf("%s <factory-name> [<properties>]", cmd);
		return false;
	}
	if (n == 2)
		props = parse_props(a[1]);

	proxy = pw_core_create_object(rd->core, a[0],
					    PW_TYPE_INTERFACE_Device,
					    PW_VERSION_DEVICE,
					    props ? &props->dict : NULL,
					    sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
	pd->destroy = (pw_destroy_t) pw_device_info_free;
	pw_proxy_add_object_listener(proxy, &pd->object_listener, &device_events, pd);
	pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id(proxy));

	return true;
}

static bool do_create_node(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[2];
        int n;
	uint32_t id;
	struct pw_proxy *proxy;
	struct pw_properties *props = NULL;
	struct proxy_data *pd;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		*error = spa_aprintf("%s <factory-name> [<properties>]", cmd);
		return false;
	}
	if (n == 2)
		props = parse_props(a[1]);

	proxy = pw_core_create_object(rd->core, a[0],
					    PW_TYPE_INTERFACE_Node,
					    PW_VERSION_NODE,
					    props ? &props->dict : NULL,
					    sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
        pd->destroy = (pw_destroy_t) pw_node_info_free;
        pw_proxy_add_object_listener(proxy, &pd->object_listener, &node_events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id(proxy));

	return true;
}

static bool do_destroy(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[1];
        int n;
	uint32_t id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		*error = spa_aprintf("%s <object-id>", cmd);
		return false;
	}
	id = atoi(a[0]);
	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global %d", cmd, id);
		return false;
	}
	pw_registry_destroy(rd->registry, id);

	return true;
}

static bool do_create_link(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[5];
        int n;
	uint32_t id;
	struct pw_proxy *proxy;
	struct pw_properties *props = NULL;
	struct proxy_data *pd;

	n = pw_split_ip(args, WHITESPACE, 5, a);
	if (n < 4) {
		*error = spa_aprintf("%s <node-id> <port> <node-id> <port> [<properties>]", cmd);
		return false;
	}
	if (n == 5)
		props = parse_props(a[4]);
	else
		props = pw_properties_new(NULL, NULL);

	pw_properties_set(props, PW_KEY_LINK_OUTPUT_NODE, a[0]);
	pw_properties_set(props, PW_KEY_LINK_OUTPUT_PORT, a[1]);
	pw_properties_set(props, PW_KEY_LINK_INPUT_NODE, a[2]);
	pw_properties_set(props, PW_KEY_LINK_INPUT_PORT, a[3]);

	proxy = (struct pw_proxy*)pw_core_create_object(rd->core,
					  "link-factory",
					  PW_TYPE_INTERFACE_Link,
					  PW_VERSION_LINK,
					  props ? &props->dict : NULL,
					  sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
        pd->destroy = (pw_destroy_t) pw_link_info_free;
        pw_proxy_add_object_listener(proxy, &pd->object_listener, &link_events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id((struct pw_proxy*)proxy));

	return true;
}

static bool do_export_node(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	struct pw_global *global;
	struct pw_node *node;
	struct pw_proxy *proxy;
	char *a[2];
	int n, idx;
	uint32_t id;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		*error = spa_aprintf("%s <node-id> [<remote-var>]", cmd);
		return false;
	}
	if (n == 2) {
		idx = atoi(a[1]);
		rd = pw_map_lookup(&data->vars, idx);
		if (rd == NULL)
			goto no_remote;
	}

	global = pw_context_find_global(data->context, atoi(a[0]));
	if (global == NULL) {
		*error = spa_aprintf("object %d does not exist", atoi(a[0]));
		return false;
	}
	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node)) {
		*error = spa_aprintf("object %d is not a node", atoi(a[0]));
		return false;
	}
	node = pw_global_get_object(global);
	proxy = pw_core_export(rd->core, PW_TYPE_INTERFACE_Node, NULL, node, 0);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id((struct pw_proxy*)proxy));

	return true;

      no_remote:
        *error = spa_aprintf("Remote %d does not exist", idx);
	return false;
}

static bool do_enum_params(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[2];
        int n;
	uint32_t id, param_id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 2) {
		*error = spa_aprintf("%s <object-id> <param-id>", cmd);
		return false;
	}

	id = atoi(a[0]);
	param_id = atoi(a[1]);

	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global %d", cmd, id);
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}

	if (strcmp(global->type, PW_TYPE_INTERFACE_Node) == 0)
		pw_node_enum_params((struct pw_node*)global->proxy, 0,
			param_id, 0, 0, NULL);
	else if (strcmp(global->type, PW_TYPE_INTERFACE_Port) == 0)
		pw_port_enum_params((struct pw_port*)global->proxy, 0,
			param_id, 0, 0, NULL);
	else if (strcmp(global->type, PW_TYPE_INTERFACE_Device) == 0)
		pw_device_enum_params((struct pw_device*)global->proxy, 0,
			param_id, 0, 0, NULL);
	else if (strcmp(global->type, PW_TYPE_INTERFACE_Endpoint) == 0)
		pw_endpoint_enum_params((struct pw_endpoint*)global->proxy, 0,
			param_id, 0, 0, NULL);
	else {
		*error = spa_aprintf("enum-params not implemented on object %d type:%s",
				atoi(a[0]), global->type);
		return false;
	}
	return true;
}

static bool do_permissions(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[3];
        int n;
	uint32_t id;
	struct global *global;
	struct pw_permission permissions[1];

	n = pw_split_ip(args, WHITESPACE, 3, a);
	if (n < 3) {
		*error = spa_aprintf("%s <client-id> <object> <permission>", cmd);
		return false;
	}

	id = atoi(a[0]);
	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global %d", cmd, id);
		return false;
	}
	if (strcmp(global->type, PW_TYPE_INTERFACE_Client) != 0) {
		*error = spa_aprintf("object %d is not a client", atoi(a[0]));
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}

	permissions[0] = PW_PERMISSION_INIT(atoi(a[1]), atoi(a[2]));

	pw_client_update_permissions((struct pw_client*)global->proxy,
			1, permissions);

	return true;
}

static bool do_get_permissions(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[3];
        int n;
	uint32_t id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		*error = spa_aprintf("%s <client-id>", cmd);
		return false;
	}

	id = atoi(a[0]);
	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global %d", cmd, id);
		return false;
	}
	if (strcmp(global->type, PW_TYPE_INTERFACE_Client) != 0) {
		*error = spa_aprintf("object %d is not a client", atoi(a[0]));
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}
	pw_client_get_permissions((struct pw_client*)global->proxy,
			0, UINT32_MAX);

	return true;
}

static bool parse(struct data *data, char *buf, size_t size, char **error)
{
	char *a[2];
	int n;
	size_t i;
	char *p, *cmd, *args;

	if ((p = strchr(buf, '#')))
		*p = '\0';

	p = pw_strip(buf, "\n\r \t");

	if (*p == '\0')
		return true;

	n = pw_split_ip(p, WHITESPACE, 2, a);
	if (n < 1)
		return true;

	cmd = a[0];
	args = n > 1 ? a[1] : "";

	for (i = 0; i < SPA_N_ELEMENTS(command_list); i++) {
		if (!strcmp(command_list[i].name, cmd) ||
		    !strcmp(command_list[i].alias, cmd)) {
			return command_list[i].func(data, cmd, args, error);
		}
	}
        *error = spa_aprintf("Command \"%s\" does not exist. Type 'help' for usage.", cmd);
	return false;
}

static void do_input(void *data, int fd, uint32_t mask)
{
	struct data *d = data;
	char buf[4096], *error;
	ssize_t r;

	if (mask & SPA_IO_IN) {
		while (true) {
			r = read(fd, buf, sizeof(buf));
			if (r < 0) {
				if (errno == EAGAIN)
					continue;
				perror("read");
				r = 0;
				break;
			}
			break;
		}
		if (r == 0) {
			fprintf(stdout, "\n");
			pw_main_loop_quit(d->loop);
			return;
		}
		buf[r] = '\0';

		if (!parse(d, buf, r, &error)) {
			fprintf(stdout, "Error: \"%s\"\n", error);
			free(error);
		}
		if (d->current == NULL)
			pw_main_loop_quit(d->loop);
		else  {
			struct remote_data *rd = d->current;
			if (rd->core)
				rd->prompt_pending = pw_core_sync(rd->core, 0, 0);
		}
	}
}

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	char *error;

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	spa_list_init(&data.remotes);
	pw_map_init(&data.vars, 64, 16);

	data.context = pw_context_new(l, pw_properties_new(PW_KEY_CORE_DAEMON, "1", NULL), 0);

	pw_context_load_module(data.context, "libpipewire-module-link-factory", NULL, NULL);

	pw_loop_add_io(l, STDIN_FILENO, SPA_IO_IN|SPA_IO_HUP, false, do_input, &data);

	fprintf(stdout, "Welcome to PipeWire version %s. Type 'help' for usage.\n",
			pw_get_library_version());

	do_connect(&data, "connect", "internal", &error);

	pw_main_loop_run(data.loop);

	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);

	return 0;
}
