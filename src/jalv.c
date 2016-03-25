/*
  Copyright 2007-2015 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#define _POSIX_C_SOURCE 200809L  /* for mkdtemp */
#define _DARWIN_C_SOURCE /* for mkdtemp on OSX */
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __cplusplus
#    include <stdbool.h>
#endif

#ifdef _WIN32
#    include <io.h>  /* for _mktemp */
#    define snprintf _snprintf
#else
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

#include "jalv_config.h"
#include "jalv_internal.h"

#include <jack/jack.h>
#include <jack/midiport.h>
#ifdef JALV_JACK_SESSION
#    include <jack/session.h>
#endif
#ifdef HAVE_JACK_METADATA
#    include <jack/metadata.h>
#endif

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/buf-size/buf-size.h"
#include "lv2/lv2plug.in/ns/ext/data-access/data-access.h"
#include "lv2/lv2plug.in/ns/ext/event/event.h"
#include "lv2/lv2plug.in/ns/ext/options/options.h"
#include "lv2/lv2plug.in/ns/ext/parameters/parameters.h"
#include "lv2/lv2plug.in/ns/ext/patch/patch.h"
#include "lv2/lv2plug.in/ns/ext/port-groups/port-groups.h"
#include "lv2/lv2plug.in/ns/ext/port-props/port-props.h"
#include "lv2/lv2plug.in/ns/ext/presets/presets.h"
#include "lv2/lv2plug.in/ns/ext/state/state.h"
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include "lv2/lv2plug.in/ns/ext/uri-map/uri-map.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/ext/worker/worker.h"
#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"

#include "lilv/lilv.h"

#include "suil/suil.h"

#include "lv2_evbuf.h"
#include "worker.h"

#define NS_RDF "http://www.w3.org/1999/02/22-rdf-syntax-ns#"

#ifndef MIN
#    define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#    define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifdef __clang__
#    define REALTIME __attribute__((annotate("realtime")))
#else
#    define REALTIME
#endif

/* Size factor for UI ring buffers.  The ring size is a few times the size of
   an event output to give the UI a chance to keep up.  Experiments with Ingen,
   which can highly saturate its event output, led me to this value.  It
   really ought to be enough for anybody(TM).
*/
#define N_BUFFER_CYCLES 16

ZixSem exit_sem;  /**< Exit semaphore */

static LV2_URID
map_uri(LV2_URID_Map_Handle handle,
        const char*         uri)
{
	Jalv* jalv = (Jalv*)handle;
	zix_sem_wait(&jalv->symap_lock);
	const LV2_URID id = symap_map(jalv->symap, uri);
	zix_sem_post(&jalv->symap_lock);
	return id;
}

static const char*
unmap_uri(LV2_URID_Unmap_Handle handle,
          LV2_URID              urid)
{
	Jalv* jalv = (Jalv*)handle;
	zix_sem_wait(&jalv->symap_lock);
	const char* uri = symap_unmap(jalv->symap, urid);
	zix_sem_post(&jalv->symap_lock);
	return uri;
}

/**
   Map function for URI map extension.
*/
static uint32_t
uri_to_id(LV2_URI_Map_Callback_Data callback_data,
          const char*               map,
          const char*               uri)
{
	Jalv* jalv = (Jalv*)callback_data;
	zix_sem_wait(&jalv->symap_lock);
	const LV2_URID id = symap_map(jalv->symap, uri);
	zix_sem_post(&jalv->symap_lock);
	return id;
}

#define NS_EXT "http://lv2plug.in/ns/ext/"

static LV2_URI_Map_Feature uri_map = { NULL, &uri_to_id };

static LV2_Extension_Data_Feature ext_data = { NULL };

static LV2_Feature uri_map_feature   = { NS_EXT "uri-map", &uri_map };
static LV2_Feature map_feature       = { LV2_URID__map, NULL };
static LV2_Feature unmap_feature     = { LV2_URID__unmap, NULL };
static LV2_Feature make_path_feature = { LV2_STATE__makePath, NULL };
static LV2_Feature schedule_feature  = { LV2_WORKER__schedule, NULL };
static LV2_Feature log_feature       = { LV2_LOG__log, NULL };
static LV2_Feature options_feature   = { LV2_OPTIONS__options, NULL };
static LV2_Feature def_state_feature = { LV2_STATE__loadDefaultState, NULL };

/** These features have no data */
static LV2_Feature buf_size_features[3] = {
	{ LV2_BUF_SIZE__powerOf2BlockLength, NULL },
	{ LV2_BUF_SIZE__fixedBlockLength, NULL },
	{ LV2_BUF_SIZE__boundedBlockLength, NULL } };

const LV2_Feature* features[13] = {
	&uri_map_feature, &map_feature, &unmap_feature,
	&make_path_feature,
	&schedule_feature,
	&log_feature,
	&options_feature,
	&def_state_feature,
	&buf_size_features[0],
	&buf_size_features[1],
	&buf_size_features[2],
	NULL
};

/** Return true iff Jalv supports the given feature. */
static bool
feature_is_supported(const char* uri)
{
	if (!strcmp(uri, "http://lv2plug.in/ns/lv2core#isLive")) {
		return true;
	}
	for (const LV2_Feature*const* f = features; *f; ++f) {
		if (!strcmp(uri, (*f)->URI)) {
			return true;
		}
	}
	return false;
}

/** Abort and exit on error */
static void
die(const char* msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}

/**
   Create a port structure from data description.  This is called before plugin
   and Jack instantiation.  The remaining instance-specific setup
   (e.g. buffers) is done later in activate_port().
*/
static void
create_port(Jalv*    jalv,
            uint32_t port_index,
            float    default_value)
{
	struct Port* const port = &jalv->ports[port_index];

	port->lilv_port = lilv_plugin_get_port_by_index(jalv->plugin, port_index);
	port->jack_port = NULL;
	port->evbuf     = NULL;
	port->buf_size  = 0;
	port->index     = port_index;
	port->control   = 0.0f;
	port->flow      = FLOW_UNKNOWN;

	const bool optional = lilv_port_has_property(
		jalv->plugin, port->lilv_port, jalv->nodes.lv2_connectionOptional);

	/* Set the port flow (input or output) */
	if (lilv_port_is_a(jalv->plugin, port->lilv_port, jalv->nodes.lv2_InputPort)) {
		port->flow = FLOW_INPUT;
	} else if (lilv_port_is_a(jalv->plugin, port->lilv_port,
	                          jalv->nodes.lv2_OutputPort)) {
		port->flow = FLOW_OUTPUT;
	} else if (!optional) {
		die("Mandatory port has unknown type (neither input nor output)");
	}

	/* Set control values */
	if (lilv_port_is_a(jalv->plugin, port->lilv_port, jalv->nodes.lv2_ControlPort)) {
		port->type    = TYPE_CONTROL;
		port->control = isnan(default_value) ? 0.0f : default_value;
	} else if (lilv_port_is_a(jalv->plugin, port->lilv_port,
	                          jalv->nodes.lv2_AudioPort)) {
		port->type = TYPE_AUDIO;
	} else if (lilv_port_is_a(jalv->plugin, port->lilv_port,
	                          jalv->nodes.ev_EventPort)) {
		port->type = TYPE_EVENT;
		port->old_api = true;
	} else if (lilv_port_is_a(jalv->plugin, port->lilv_port,
	                          jalv->nodes.atom_AtomPort)) {
		port->type = TYPE_EVENT;
		port->old_api = false;
	} else if (!optional) {
		die("Mandatory port has unknown data type");
	}

	LilvNode* min_size = lilv_port_get(
		jalv->plugin, port->lilv_port, jalv->nodes.rsz_minimumSize);
	if (min_size && lilv_node_is_int(min_size)) {
		port->buf_size = lilv_node_as_int(min_size);
		jalv->opts.buffer_size = MAX(
			jalv->opts.buffer_size, port->buf_size * N_BUFFER_CYCLES);
	}
	lilv_node_free(min_size);

	/* Update longest symbol for aligned console printing */
	const LilvNode* sym = lilv_port_get_symbol(jalv->plugin, port->lilv_port);
	const size_t    len = strlen(lilv_node_as_string(sym));
	if (len > jalv->longest_sym) {
		jalv->longest_sym = len;
	}
}

/**
   Create port structures from data (via create_port()) for all ports.
*/
void
jalv_create_ports(Jalv* jalv)
{
	jalv->num_ports = lilv_plugin_get_num_ports(jalv->plugin);
	jalv->ports     = (struct Port*)calloc(jalv->num_ports, sizeof(struct Port));
	float* default_values = (float*)calloc(
		lilv_plugin_get_num_ports(jalv->plugin), sizeof(float));
	lilv_plugin_get_port_ranges_float(jalv->plugin, NULL, NULL, default_values);

	for (uint32_t i = 0; i < jalv->num_ports; ++i) {
		create_port(jalv, i, default_values[i]);
	}

	const LilvPort* control_input = lilv_plugin_get_port_by_designation(
		jalv->plugin, jalv->nodes.lv2_InputPort, jalv->nodes.lv2_control);
	if (control_input) {
		jalv->control_in = lilv_port_get_index(jalv->plugin, control_input);
	}

	free(default_values);
}

/**
   Allocate port buffers (only necessary for MIDI).
*/
static void
jalv_allocate_port_buffers(Jalv* jalv)
{
	for (uint32_t i = 0; i < jalv->num_ports; ++i) {
		struct Port* const port = &jalv->ports[i];
		switch (port->type) {
		case TYPE_EVENT:
			lv2_evbuf_free(port->evbuf);
			const size_t buf_size = (port->buf_size > 0)
				? port->buf_size
				: jalv->midi_buf_size;
			port->evbuf = lv2_evbuf_new(
				buf_size,
				port->old_api ? LV2_EVBUF_EVENT : LV2_EVBUF_ATOM,
				jalv->map.map(jalv->map.handle,
				              lilv_node_as_string(jalv->nodes.atom_Chunk)),
				jalv->map.map(jalv->map.handle,
				              lilv_node_as_string(jalv->nodes.atom_Sequence)));
			lilv_instance_connect_port(
				jalv->instance, i, lv2_evbuf_get_buffer(port->evbuf));
		default: break;
		}
	}
}

/**
   Get a port structure by symbol.

   TODO: Build an index to make this faster, currently O(n) which may be
   a problem when restoring the state of plugins with many ports.
*/
struct Port*
jalv_port_by_symbol(Jalv* jalv, const char* sym)
{
	for (uint32_t i = 0; i < jalv->num_ports; ++i) {
		struct Port* const port     = &jalv->ports[i];
		const LilvNode*    port_sym = lilv_port_get_symbol(jalv->plugin,
		                                                   port->lilv_port);

		if (!strcmp(lilv_node_as_string(port_sym), sym)) {
			return port;
		}
	}

	return NULL;
}

static void
print_control_value(Jalv* jalv, const struct Port* port, float value)
{
	const LilvNode* sym = lilv_port_get_symbol(jalv->plugin, port->lilv_port);
	printf("%-*s = %f\n", jalv->longest_sym, lilv_node_as_string(sym), value);
}

/**
   Expose a port to Jack (if applicable) and connect it to its buffer.
*/
static void
activate_port(Jalv*    jalv,
              uint32_t port_index)
{
	struct Port* const port = &jalv->ports[port_index];

	const LilvNode* sym = lilv_port_get_symbol(jalv->plugin, port->lilv_port);

	/* Connect unsupported ports to NULL (known to be optional by this point) */
	if (port->flow == FLOW_UNKNOWN || port->type == TYPE_UNKNOWN) {
		lilv_instance_connect_port(jalv->instance, port_index, NULL);
		return;
	}

	/* Build Jack flags for port */
	enum JackPortFlags jack_flags = (port->flow == FLOW_INPUT)
		? JackPortIsInput
		: JackPortIsOutput;

	/* Connect the port based on its type */
	switch (port->type) {
	case TYPE_CONTROL:
		print_control_value(jalv, port, port->control);
		lilv_instance_connect_port(jalv->instance, port_index, &port->control);
		break;
	case TYPE_AUDIO:
		port->jack_port = jack_port_register(
			jalv->jack_client, lilv_node_as_string(sym),
			JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
		break;
	case TYPE_EVENT:
		if (lilv_port_supports_event(
			    jalv->plugin, port->lilv_port, jalv->nodes.midi_MidiEvent)) {
			port->jack_port = jack_port_register(
				jalv->jack_client, lilv_node_as_string(sym),
				JACK_DEFAULT_MIDI_TYPE, jack_flags, 0);
		}
		break;
	default:
		break;
	}

#ifdef HAVE_JACK_METADATA
	if (port->jack_port) {
		// Set port order to index
		char index_str[16];
		snprintf(index_str, sizeof(index_str), "%d", port_index);
		jack_set_property(jalv->jack_client, jack_port_uuid(port->jack_port),
		                  "http://jackaudio.org/metadata/order", index_str,
		                  "http://www.w3.org/2001/XMLSchema#integer");

		// Set port pretty name to label
		LilvNode* name = lilv_port_get_name(jalv->plugin, port->lilv_port);
		jack_set_property(jalv->jack_client, jack_port_uuid(port->jack_port),
		                  JACK_METADATA_PRETTY_NAME, lilv_node_as_string(name),
		                  "text/plain");
		lilv_node_free(name);
	}
#endif
}

/** Jack buffer size callback. */
static int
jack_buffer_size_cb(jack_nframes_t nframes, void* data)
{
	Jalv* const jalv = (Jalv*)data;
	jalv->block_length = nframes;
	jalv->buf_size_set = true;
#ifdef HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE
	jalv->midi_buf_size = jack_port_type_get_buffer_size(
		jalv->jack_client, JACK_DEFAULT_MIDI_TYPE);
#endif
	jalv_allocate_port_buffers(jalv);
	return 0;
}

/** Jack shutdown callback. */
static void
jack_shutdown_cb(void* data)
{
	Jalv* const jalv = (Jalv*)data;
	jalv_close_ui(jalv);
	zix_sem_post(jalv->done);
}

/** Jack process callback. */
static REALTIME int
jack_process_cb(jack_nframes_t nframes, void* data)
{
	Jalv* const jalv = (Jalv*)data;

	/* Get Jack transport position */
	jack_position_t pos;
	const bool rolling = (jack_transport_query(jalv->jack_client, &pos)
	                      == JackTransportRolling);

	/* If transport state is not as expected, then something has changed */
	const bool xport_changed = (rolling != jalv->rolling ||
	                            pos.frame != jalv->position ||
	                            pos.beats_per_minute != jalv->bpm);

	uint8_t   pos_buf[256];
	LV2_Atom* lv2_pos = (LV2_Atom*)pos_buf;
	if (xport_changed) {
		/* Build an LV2 position object to report change to plugin */
		lv2_atom_forge_set_buffer(&jalv->forge, pos_buf, sizeof(pos_buf));
		LV2_Atom_Forge*      forge = &jalv->forge;
		LV2_Atom_Forge_Frame frame;
		lv2_atom_forge_object(forge, &frame, 0, jalv->urids.time_Position);
		lv2_atom_forge_key(forge, jalv->urids.time_frame);
		lv2_atom_forge_long(forge, pos.frame);
		lv2_atom_forge_key(forge, jalv->urids.time_speed);
		lv2_atom_forge_float(forge, rolling ? 1.0 : 0.0);
		if (pos.valid & JackPositionBBT) {
			lv2_atom_forge_key(forge, jalv->urids.time_barBeat);
			lv2_atom_forge_float(
				forge, pos.beat - 1 + (pos.tick / pos.ticks_per_beat));
			lv2_atom_forge_key(forge, jalv->urids.time_bar);
			lv2_atom_forge_long(forge, pos.bar - 1);
			lv2_atom_forge_key(forge, jalv->urids.time_beatUnit);
			lv2_atom_forge_int(forge, pos.beat_type);
			lv2_atom_forge_key(forge, jalv->urids.time_beatsPerBar);
			lv2_atom_forge_float(forge, pos.beats_per_bar);
			lv2_atom_forge_key(forge, jalv->urids.time_beatsPerMinute);
			lv2_atom_forge_float(forge, pos.beats_per_minute);
		}

		if (jalv->opts.dump) {
			char* str = sratom_to_turtle(
				jalv->sratom, &jalv->unmap, "time:", NULL, NULL,
				lv2_pos->type, lv2_pos->size, LV2_ATOM_BODY(lv2_pos));
			printf("\n## Position\n%s\n", str);
			free(str);
		}
	}

	/* Update transport state to expected values for next cycle */
	jalv->position = rolling ? pos.frame + nframes : pos.frame;
	jalv->bpm      = pos.beats_per_minute;
	jalv->rolling  = rolling;

	switch (jalv->play_state) {
	case JALV_PAUSE_REQUESTED:
		jalv->play_state = JALV_PAUSED;
		zix_sem_post(&jalv->paused);
		break;
	case JALV_PAUSED:
		for (uint32_t p = 0; p < jalv->num_ports; ++p) {
			jack_port_t* jport = jalv->ports[p].jack_port;
			if (jport && jalv->ports[p].flow == FLOW_OUTPUT) {
				void* buf = jack_port_get_buffer(jport, nframes);
				if (jalv->ports[p].type == TYPE_EVENT) {
					jack_midi_clear_buffer(buf);
				} else {
					memset(buf, '\0', nframes * sizeof(float));
				}
			}
		}
		return 0;
	default:
		break;
	}

	/* Prepare port buffers */
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* port = &jalv->ports[p];
		if (port->type == TYPE_AUDIO && port->jack_port) {
			/* Connect plugin port directly to Jack port buffer */
			lilv_instance_connect_port(
				jalv->instance, p,
				jack_port_get_buffer(port->jack_port, nframes));

		} else if (port->type == TYPE_EVENT && port->flow == FLOW_INPUT) {
			lv2_evbuf_reset(port->evbuf, true);

			/* Write transport change event if applicable */
			LV2_Evbuf_Iterator iter = lv2_evbuf_begin(port->evbuf);
			if (xport_changed) {
				lv2_evbuf_write(
					&iter, 0, 0,
					lv2_pos->type, lv2_pos->size, LV2_ATOM_BODY(lv2_pos));
			}

			if (jalv->state_changed) {
				/* Plugin state has changed, request an update */
				const LV2_Atom_Object get = {
					{ sizeof(LV2_Atom_Object), jalv->urids.atom_Object },
					{ 0, jalv->urids.patch_Get } };

				lv2_evbuf_write(
					&iter, 0, 0,
					get.atom.type, get.atom.size, LV2_ATOM_BODY(&get));

				jalv->state_changed = false;
			}

			if (port->jack_port) {
				/* Write Jack MIDI input */
				void* buf = jack_port_get_buffer(port->jack_port, nframes);
				for (uint32_t i = 0; i < jack_midi_get_event_count(buf); ++i) {
					jack_midi_event_t ev;
					jack_midi_event_get(&ev, buf, i);
					lv2_evbuf_write(&iter,
					                ev.time, 0,
					                jalv->midi_event_id,
					                ev.size, ev.buffer);
				}
			}
		} else if (port->type == TYPE_EVENT) {
			/* Clear event output for plugin to write to */
			lv2_evbuf_reset(port->evbuf, false);
		}
	}

	/* Read and apply control change events from UI */
	if (jalv->has_ui) {
		ControlChange ev;
		const size_t  space = jack_ringbuffer_read_space(jalv->ui_events);
		for (size_t i = 0; i < space; i += sizeof(ev) + ev.size) {
			jack_ringbuffer_read(jalv->ui_events, (char*)&ev, sizeof(ev));
			char body[ev.size];
			if (jack_ringbuffer_read(jalv->ui_events, body, ev.size) != ev.size) {
				fprintf(stderr, "error: Error reading from UI ring buffer\n");
				break;
			}
			assert(ev.index < jalv->num_ports);
			struct Port* const port = &jalv->ports[ev.index];
			if (ev.protocol == 0) {
				assert(ev.size == sizeof(float));
				port->control = *(float*)body;
			} else if (ev.protocol == jalv->urids.atom_eventTransfer) {
				LV2_Evbuf_Iterator    e    = lv2_evbuf_end(port->evbuf);
				const LV2_Atom* const atom = (const LV2_Atom*)body;
				lv2_evbuf_write(&e, nframes, 0, atom->type, atom->size,
				                LV2_ATOM_BODY_CONST(atom));
			} else {
				fprintf(stderr, "error: Unknown control change protocol %d\n",
				        ev.protocol);
			}
		}
	}

	/* Run plugin for this cycle */
	lilv_instance_run(jalv->instance, nframes);

	/* Process any replies from the worker. */
	jalv_worker_emit_responses(jalv, &jalv->worker);

	/* Notify the plugin the run() cycle is finished */
	if (jalv->worker.iface && jalv->worker.iface->end_run) {
		jalv->worker.iface->end_run(jalv->instance->lv2_handle);
	}

	/* Check if it's time to send updates to the UI */
	jalv->event_delta_t += nframes;
	bool           send_ui_updates = false;
	jack_nframes_t update_frames   = jalv->sample_rate / jalv->ui_update_hz;
	if (jalv->has_ui && (jalv->event_delta_t > update_frames)) {
		send_ui_updates = true;
		jalv->event_delta_t = 0;
	}

	/* Deliver MIDI output and UI events */
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* const port = &jalv->ports[p];
		if (port->flow == FLOW_OUTPUT && port->type == TYPE_CONTROL &&
		    lilv_port_has_property(jalv->plugin, port->lilv_port,
		                           jalv->nodes.lv2_reportsLatency)) {
			if (jalv->plugin_latency != port->control) {
				jalv->plugin_latency = port->control;
				jack_recompute_total_latencies(jalv->jack_client);
			}
		}

		if (port->flow == FLOW_OUTPUT && port->type == TYPE_EVENT) {
			void* buf = NULL;
			if (port->jack_port) {
				buf = jack_port_get_buffer(port->jack_port, nframes);
				jack_midi_clear_buffer(buf);
			}

			for (LV2_Evbuf_Iterator i = lv2_evbuf_begin(port->evbuf);
			     lv2_evbuf_is_valid(i);
			     i = lv2_evbuf_next(i)) {
				uint32_t frames, subframes, type, size;
				uint8_t* body;
				lv2_evbuf_get(i, &frames, &subframes, &type, &size, &body);
				if (buf && type == jalv->midi_event_id) {
					jack_midi_event_write(buf, frames, body, size);
				}

				/* TODO: Be more disciminate about what to send */
				if (jalv->has_ui && !port->old_api) {
					char evbuf[sizeof(ControlChange) + sizeof(LV2_Atom)];
					ControlChange* ev = (ControlChange*)evbuf;
					ev->index    = p;
					ev->protocol = jalv->urids.atom_eventTransfer;
					ev->size     = sizeof(LV2_Atom) + size;
					LV2_Atom* atom = (LV2_Atom*)ev->body;
					atom->type = type;
					atom->size = size;
					if (jack_ringbuffer_write_space(jalv->plugin_events)
					    < sizeof(evbuf) + size) {
						fprintf(stderr, "Plugin => UI buffer overflow!\n");
						break;
					}
					jack_ringbuffer_write(jalv->plugin_events, evbuf, sizeof(evbuf));
					/* TODO: race, ensure reader handles this correctly */
					jack_ringbuffer_write(jalv->plugin_events, (void*)body, size);
				}
			}
		} else if (send_ui_updates
		           && port->flow != FLOW_INPUT
		           && port->type == TYPE_CONTROL) {
			char buf[sizeof(ControlChange) + sizeof(float)];
			ControlChange* ev = (ControlChange*)buf;
			ev->index    = p;
			ev->protocol = 0;
			ev->size     = sizeof(float);
			*(float*)ev->body = port->control;
			if (jack_ringbuffer_write(jalv->plugin_events, buf, sizeof(buf))
			    < sizeof(buf)) {
				fprintf(stderr, "Plugin => UI buffer overflow!\n");
			}
		}
	}

	return 0;
}

/** Calculate latency assuming all ports depend on each other. */
static void
jack_latency_cb(jack_latency_callback_mode_t mode, void* data)
{
	Jalv* const         jalv = (Jalv*)data;
	const enum PortFlow flow = ((mode == JackCaptureLatency)
	                            ? FLOW_INPUT : FLOW_OUTPUT);

	/* First calculate the min/max latency of all feeding ports */
	uint32_t             ports_found = 0;
	jack_latency_range_t range       = { UINT32_MAX, 0 };
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* port = &jalv->ports[p];
		if (port->jack_port && port->flow == flow) {
			jack_latency_range_t r;
			jack_port_get_latency_range(port->jack_port, mode, &r);
			if (r.min < range.min) { range.min = r.min; }
			if (r.max > range.max) { range.max = r.max; }
			++ports_found;
		}
	}

	if (ports_found == 0) {
		range.min = 0;
	}

	/* Add the plugin's own latency */
	range.min += jalv->plugin_latency;
	range.max += jalv->plugin_latency;

	/* Tell Jack about it */
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* port = &jalv->ports[p];
		if (port->jack_port && port->flow == flow) {
			jack_port_set_latency_range(port->jack_port, mode, &range);
		}
	}
}

#ifdef JALV_JACK_SESSION
static void
jack_session_cb(jack_session_event_t* event, void* arg)
{
	Jalv* const jalv = (Jalv*)arg;

	#define MAX_CMD_LEN 256
	event->command_line = malloc(MAX_CMD_LEN);
	snprintf(event->command_line, MAX_CMD_LEN, "%s -u %s -l \"${SESSION_DIR}\"",
	         jalv->prog_name,
	         event->client_uuid);

	switch (event->type) {
	case JackSessionSave:
	case JackSessionSaveTemplate:
		jalv_save(jalv, event->session_dir);
		break;
	case JackSessionSaveAndQuit:
		jalv_save(jalv, event->session_dir);
		jalv_close_ui(jalv);
		break;
	}

	jack_session_reply(jalv->jack_client, event);
	jack_session_event_free(event);
}
#endif /* JALV_JACK_SESSION */

void
jalv_ui_instantiate(Jalv* jalv, const char* native_ui_type, void* parent)
{
	jalv->ui_host = suil_host_new(jalv_ui_write, jalv_ui_port_index, NULL, NULL);

	const LV2_Feature parent_feature = {
		LV2_UI__parent, parent
	};
	const LV2_Feature instance_feature = {
		NS_EXT "instance-access", lilv_instance_get_handle(jalv->instance)
	};
	const LV2_Feature data_feature = {
		LV2_DATA_ACCESS_URI, &ext_data
	};
	const LV2_Feature idle_feature = {
		LV2_UI__idleInterface, NULL
	};
	const LV2_Feature* ui_features[] = {
		&uri_map_feature, &map_feature, &unmap_feature,
		&instance_feature,
		&data_feature,
		&log_feature,
		&parent_feature,
		&options_feature,
		&idle_feature,
		NULL
	};

	const char* bundle_uri  = lilv_node_as_uri(lilv_ui_get_bundle_uri(jalv->ui));
	const char* binary_uri  = lilv_node_as_uri(lilv_ui_get_binary_uri(jalv->ui));
	char*       bundle_path = lilv_file_uri_parse(bundle_uri, NULL);
	char*       binary_path = lilv_file_uri_parse(binary_uri, NULL);

	jalv->ui_instance = suil_instance_new(
		jalv->ui_host,
		jalv,
		native_ui_type,
		lilv_node_as_uri(lilv_plugin_get_uri(jalv->plugin)),
		lilv_node_as_uri(lilv_ui_get_uri(jalv->ui)),
		lilv_node_as_uri(jalv->ui_type),
		bundle_path,
		binary_path,
		ui_features);

	lilv_free(binary_path);
	lilv_free(bundle_path);

	/* Set initial control values on UI */
	if (jalv->ui_instance) {
		for (uint32_t i = 0; i < jalv->num_ports; ++i) {
			if (jalv->ports[i].type == TYPE_CONTROL) {
				suil_instance_port_event(jalv->ui_instance, i,
				                         sizeof(float), 0,
				                         &jalv->ports[i].control);
			}
		}
	}
}

bool
jalv_ui_is_resizable(Jalv* jalv)
{
	if (!jalv->ui) {
		return false;
	}

	const LilvNode* s   = lilv_ui_get_uri(jalv->ui);
	LilvNode*       p   = lilv_new_uri(jalv->world, LV2_CORE__optionalFeature);
	LilvNode*       fs  = lilv_new_uri(jalv->world, LV2_UI__fixedSize);
	LilvNode*       nrs = lilv_new_uri(jalv->world, LV2_UI__noUserResize);

	LilvNodes* fs_matches = lilv_world_find_nodes(jalv->world, s, p, fs);
	LilvNodes* nrs_matches = lilv_world_find_nodes(jalv->world, s, p, nrs);

	lilv_nodes_free(nrs_matches);
	lilv_nodes_free(fs_matches);
	lilv_node_free(nrs);
	lilv_node_free(fs);
	lilv_node_free(p);

	return !fs_matches && !nrs_matches;
}

void
jalv_ui_write(SuilController controller,
              uint32_t       port_index,
              uint32_t       buffer_size,
              uint32_t       protocol,
              const void*    buffer)
{
	Jalv* const jalv = (Jalv*)controller;

	if (protocol != 0 && protocol != jalv->urids.atom_eventTransfer) {
		fprintf(stderr, "UI write with unsupported protocol %d (%s)\n",
		        protocol, unmap_uri(jalv, protocol));
		return;
	}

	if (port_index >= jalv->num_ports) {
		fprintf(stderr, "UI write to out of range port index %d\n",
		        port_index);
		return;
	}

	if (jalv->opts.dump && protocol == jalv->urids.atom_eventTransfer) {
		const LV2_Atom* atom = (const LV2_Atom*)buffer;
		char*           str  = sratom_to_turtle(
			jalv->sratom, &jalv->unmap, "jalv:", NULL, NULL,
			atom->type, atom->size, LV2_ATOM_BODY_CONST(atom));
		jalv_ansi_start(stdout, 36);
		printf("\n## UI => Plugin (%u bytes) ##\n%s\n", atom->size, str);
		jalv_ansi_reset(stdout);
		free(str);
	}

	char buf[sizeof(ControlChange) + buffer_size];
	ControlChange* ev = (ControlChange*)buf;
	ev->index    = port_index;
	ev->protocol = protocol;
	ev->size     = buffer_size;
	memcpy(ev->body, buffer, buffer_size);
	jack_ringbuffer_write(jalv->ui_events, buf, sizeof(buf));
}

uint32_t
jalv_ui_port_index(SuilController controller, const char* symbol)
{
	Jalv* const  jalv = (Jalv*)controller;
	struct Port* port = jalv_port_by_symbol(jalv, symbol);

	return port ? port->index : LV2UI_INVALID_PORT_INDEX;
}

bool
jalv_update(Jalv* jalv)
{
	/* Check quit flag and close if set. */
	if (zix_sem_try_wait(&exit_sem)) {
		jalv_close_ui(jalv);
		return false;
	}

	/* Emit UI events. */
	ControlChange ev;
	const size_t  space = jack_ringbuffer_read_space(jalv->plugin_events);
	for (size_t i = 0;
	     i + sizeof(ev) + sizeof(float) <= space;
	     i += sizeof(ev) + ev.size) {
		/* Read event header to get the size */
		jack_ringbuffer_read(jalv->plugin_events, (char*)&ev, sizeof(ev));

		/* Resize read buffer if necessary */
		jalv->ui_event_buf = realloc(jalv->ui_event_buf, ev.size);
		void* const buf = jalv->ui_event_buf;

		/* Read event body */
		jack_ringbuffer_read(jalv->plugin_events, buf, ev.size);

		if (jalv->opts.dump && ev.protocol == jalv->urids.atom_eventTransfer) {
			/* Dump event in Turtle to the console */
			LV2_Atom* atom = (LV2_Atom*)buf;
			char*     str  = sratom_to_turtle(
				jalv->ui_sratom, &jalv->unmap, "jalv:", NULL, NULL,
				atom->type, atom->size, LV2_ATOM_BODY(atom));
			jalv_ansi_start(stdout, 35);
			printf("\n## Plugin => UI (%u bytes) ##\n%s\n", atom->size, str);
			jalv_ansi_reset(stdout);
			free(str);
		}

		if (jalv->ui_instance) {
			suil_instance_port_event(jalv->ui_instance, ev.index,
			                         ev.size, ev.protocol, buf);
		} else {
			jalv_ui_port_event(jalv, ev.index, ev.size, ev.protocol, buf);
		}

		if (ev.protocol == 0 && jalv->opts.print_controls) {
			print_control_value(jalv, &jalv->ports[ev.index], *(float*)buf);
		}
	}

	return true;
}

static bool
jalv_apply_control_arg(Jalv* jalv, const char* s)
{
	char  sym[256];
	float val = 0.0f;
	if (sscanf(s, "%[^=]=%f", sym, &val) != 2) {
		fprintf(stderr, "warning: Ignoring invalid value `%s'\n", s);
		return false;
	}

	struct Port* port = jalv_port_by_symbol(jalv, sym);
	if (!port) {
		fprintf(stderr, "warning: Ignoring value for unknown port `%s'\n", sym);
		return false;
	}

	port->control = val;
	return true;
}

static void
signal_handler(int ignored)
{
	zix_sem_post(&exit_sem);
}

int
main(int argc, char** argv)
{
	Jalv jalv;
	memset(&jalv, '\0', sizeof(Jalv));
	jalv.prog_name     = argv[0];
	jalv.block_length  = 4096;  /* Should be set by jack_buffer_size_cb */
	jalv.midi_buf_size = 1024;  /* Should be set by jack_buffer_size_cb */
	jalv.play_state    = JALV_PAUSED;
	jalv.bpm           = 120.0f;

	if (jalv_init(&argc, &argv, &jalv.opts)) {
		return EXIT_FAILURE;
	}

	if (jalv.opts.uuid) {
		printf("UUID: %s\n", jalv.opts.uuid);
	}

	jalv.symap = symap_new();
	zix_sem_init(&jalv.symap_lock, 1);
	uri_map.callback_data = &jalv;

	jalv.map.handle  = &jalv;
	jalv.map.map     = map_uri;
	map_feature.data = &jalv.map;

	jalv.unmap.handle  = &jalv;
	jalv.unmap.unmap   = unmap_uri;
	unmap_feature.data = &jalv.unmap;

	lv2_atom_forge_init(&jalv.forge, &jalv.map);

	jalv.sratom    = sratom_new(&jalv.map);
	jalv.ui_sratom = sratom_new(&jalv.map);

	jalv.midi_event_id = uri_to_id(
		&jalv, "http://lv2plug.in/ns/ext/event", LV2_MIDI__MidiEvent);

	jalv.urids.atom_Float           = symap_map(jalv.symap, LV2_ATOM__Float);
	jalv.urids.atom_Int             = symap_map(jalv.symap, LV2_ATOM__Int);
	jalv.urids.atom_Object          = symap_map(jalv.symap, LV2_ATOM__Object);
	jalv.urids.atom_Path            = symap_map(jalv.symap, LV2_ATOM__Path);
	jalv.urids.atom_String          = symap_map(jalv.symap, LV2_ATOM__String);
	jalv.urids.atom_eventTransfer   = symap_map(jalv.symap, LV2_ATOM__eventTransfer);
	jalv.urids.bufsz_maxBlockLength = symap_map(jalv.symap, LV2_BUF_SIZE__maxBlockLength);
	jalv.urids.bufsz_minBlockLength = symap_map(jalv.symap, LV2_BUF_SIZE__minBlockLength);
	jalv.urids.bufsz_sequenceSize   = symap_map(jalv.symap, LV2_BUF_SIZE__sequenceSize);
	jalv.urids.log_Trace            = symap_map(jalv.symap, LV2_LOG__Trace);
	jalv.urids.midi_MidiEvent       = symap_map(jalv.symap, LV2_MIDI__MidiEvent);
	jalv.urids.param_sampleRate     = symap_map(jalv.symap, LV2_PARAMETERS__sampleRate);
	jalv.urids.patch_Get            = symap_map(jalv.symap, LV2_PATCH__Get);
	jalv.urids.patch_Put            = symap_map(jalv.symap, LV2_PATCH__Put);
	jalv.urids.patch_Set            = symap_map(jalv.symap, LV2_PATCH__Set);
	jalv.urids.patch_body           = symap_map(jalv.symap, LV2_PATCH__body);
	jalv.urids.patch_property       = symap_map(jalv.symap, LV2_PATCH__property);
	jalv.urids.patch_value          = symap_map(jalv.symap, LV2_PATCH__value);
	jalv.urids.time_Position        = symap_map(jalv.symap, LV2_TIME__Position);
	jalv.urids.time_bar             = symap_map(jalv.symap, LV2_TIME__bar);
	jalv.urids.time_barBeat         = symap_map(jalv.symap, LV2_TIME__barBeat);
	jalv.urids.time_beatUnit        = symap_map(jalv.symap, LV2_TIME__beatUnit);
	jalv.urids.time_beatsPerBar     = symap_map(jalv.symap, LV2_TIME__beatsPerBar);
	jalv.urids.time_beatsPerMinute  = symap_map(jalv.symap, LV2_TIME__beatsPerMinute);
	jalv.urids.time_frame           = symap_map(jalv.symap, LV2_TIME__frame);
	jalv.urids.time_speed           = symap_map(jalv.symap, LV2_TIME__speed);
	jalv.urids.ui_updateRate        = symap_map(jalv.symap, LV2_UI__updateRate);

#ifdef _WIN32
	jalv.temp_dir = jalv_strdup("jalvXXXXXX");
	_mktemp(jalv.temp_dir);
#else
	char* templ = jalv_strdup("/tmp/jalv-XXXXXX");
	jalv.temp_dir = jalv_strjoin(mkdtemp(templ), "/");
	free(templ);
#endif

	LV2_State_Make_Path make_path = { &jalv, jalv_make_path };
	make_path_feature.data = &make_path;

	LV2_Worker_Schedule schedule = { &jalv, jalv_worker_schedule };
	schedule_feature.data = &schedule;

	LV2_Log_Log llog = { &jalv, jalv_printf, jalv_vprintf };
	log_feature.data = &llog;

	zix_sem_init(&exit_sem, 0);
	jalv.done = &exit_sem;

	zix_sem_init(&jalv.paused, 0);
	zix_sem_init(&jalv.worker.sem, 0);

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Find all installed plugins */
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);
	jalv.world = world;
	const LilvPlugins* plugins = lilv_world_get_all_plugins(world);

	/* Cache URIs for concepts we'll use */
	jalv.nodes.atom_AtomPort          = lilv_new_uri(world, LV2_ATOM__AtomPort);
	jalv.nodes.atom_Chunk             = lilv_new_uri(world, LV2_ATOM__Chunk);
	jalv.nodes.atom_Float             = lilv_new_uri(world, LV2_ATOM__Float);
	jalv.nodes.atom_Path              = lilv_new_uri(world, LV2_ATOM__Path);
	jalv.nodes.atom_Sequence          = lilv_new_uri(world, LV2_ATOM__Sequence);
	jalv.nodes.ev_EventPort           = lilv_new_uri(world, LV2_EVENT__EventPort);
	jalv.nodes.lv2_AudioPort          = lilv_new_uri(world, LV2_CORE__AudioPort);
	jalv.nodes.lv2_ControlPort        = lilv_new_uri(world, LV2_CORE__ControlPort);
	jalv.nodes.lv2_InputPort          = lilv_new_uri(world, LV2_CORE__InputPort);
	jalv.nodes.lv2_OutputPort         = lilv_new_uri(world, LV2_CORE__OutputPort);
	jalv.nodes.lv2_connectionOptional = lilv_new_uri(world, LV2_CORE__connectionOptional);
	jalv.nodes.lv2_control            = lilv_new_uri(world, LV2_CORE__control);
	jalv.nodes.lv2_default            = lilv_new_uri(world, LV2_CORE__default);
	jalv.nodes.lv2_enumeration        = lilv_new_uri(world, LV2_CORE__enumeration);
	jalv.nodes.lv2_integer            = lilv_new_uri(world, LV2_CORE__integer);
	jalv.nodes.lv2_maximum            = lilv_new_uri(world, LV2_CORE__maximum);
	jalv.nodes.lv2_minimum            = lilv_new_uri(world, LV2_CORE__minimum);
	jalv.nodes.lv2_name               = lilv_new_uri(world, LV2_CORE__name);
	jalv.nodes.lv2_reportsLatency     = lilv_new_uri(world, LV2_CORE__reportsLatency);
	jalv.nodes.lv2_sampleRate         = lilv_new_uri(world, LV2_CORE__sampleRate);
	jalv.nodes.lv2_toggled            = lilv_new_uri(world, LV2_CORE__toggled);
	jalv.nodes.midi_MidiEvent         = lilv_new_uri(world, LV2_MIDI__MidiEvent);
	jalv.nodes.pg_group               = lilv_new_uri(world, LV2_PORT_GROUPS__group);
	jalv.nodes.pprops_logarithmic     = lilv_new_uri(world, LV2_PORT_PROPS__logarithmic);
	jalv.nodes.pset_Preset            = lilv_new_uri(world, LV2_PRESETS__Preset);
	jalv.nodes.pset_bank              = lilv_new_uri(world, LV2_PRESETS__bank);
	jalv.nodes.rdfs_comment           = lilv_new_uri(world, LILV_NS_RDFS "comment");
	jalv.nodes.rdfs_label             = lilv_new_uri(world, LILV_NS_RDFS "label");
	jalv.nodes.rdfs_range             = lilv_new_uri(world, LILV_NS_RDFS "range");
	jalv.nodes.rsz_minimumSize        = lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize);
	jalv.nodes.work_interface         = lilv_new_uri(world, LV2_WORKER__interface);
	jalv.nodes.work_schedule          = lilv_new_uri(world, LV2_WORKER__schedule);
	jalv.nodes.end                    = NULL;

	/* Get plugin URI from loaded state or command line */
	LilvState* state      = NULL;
	LilvNode*  plugin_uri = NULL;
	if (jalv.opts.load) {
		struct stat info;
		stat(jalv.opts.load, &info);
		if (S_ISDIR(info.st_mode)) {
			char* path = jalv_strjoin(jalv.opts.load, "/state.ttl");
			state = lilv_state_new_from_file(jalv.world, &jalv.map, NULL, path);
			free(path);
		} else {
			state = lilv_state_new_from_file(jalv.world, &jalv.map, NULL,
			                                 jalv.opts.load);
		}
		if (!state) {
			fprintf(stderr, "Failed to load state from %s\n", jalv.opts.load);
			return EXIT_FAILURE;
		}
		plugin_uri = lilv_node_duplicate(lilv_state_get_plugin_uri(state));
	} else if (argc > 1) {
		plugin_uri = lilv_new_uri(world, argv[argc - 1]);
	}

	if (!plugin_uri) {
		fprintf(stderr, "Missing plugin URI, try lv2ls to list plugins\n");
		return EXIT_FAILURE;
	}

	/* Find plugin */
	printf("Plugin:       %s\n", lilv_node_as_string(plugin_uri));
	jalv.plugin = lilv_plugins_get_by_uri(plugins, plugin_uri);
	lilv_node_free(plugin_uri);
	if (!jalv.plugin) {
		fprintf(stderr, "Failed to find plugin\n");
		lilv_world_free(world);
		return EXIT_FAILURE;
	}

	/* Load preset, if specified */
	if (jalv.opts.preset) {
		LilvNode* preset = lilv_new_uri(jalv.world, jalv.opts.preset);

		jalv_load_presets(&jalv, NULL, NULL);
		state = lilv_state_new_from_world(jalv.world, &jalv.map, preset);
		jalv.preset = state;
		lilv_node_free(preset);
		if (!state) {
			fprintf(stderr, "Failed to find preset <%s>\n", jalv.opts.preset);
			lilv_world_free(world);
			return EXIT_FAILURE;
		}
	}

	/* Check that any required features are supported */
	LilvNodes* req_feats = lilv_plugin_get_required_features(jalv.plugin);
	LILV_FOREACH(nodes, f, req_feats) {
		const char* uri = lilv_node_as_uri(lilv_nodes_get(req_feats, f));
		if (!feature_is_supported(uri)) {
			fprintf(stderr, "Feature %s is not supported\n", uri);
			lilv_world_free(world);
			return EXIT_FAILURE;
		}
	}
	lilv_nodes_free(req_feats);

	if (!state) {
		/* Not restoring state, load the plugin as a preset to get default */
		state = lilv_state_new_from_world(
			jalv.world, &jalv.map, lilv_plugin_get_uri(jalv.plugin));
	}

	/* Get a plugin UI */
	const char* native_ui_type_uri = jalv_native_ui_type(&jalv);
	jalv.uis = lilv_plugin_get_uis(jalv.plugin);
	if (!jalv.opts.generic_ui && native_ui_type_uri) {
		const LilvNode* native_ui_type = lilv_new_uri(jalv.world, native_ui_type_uri);
		LILV_FOREACH(uis, u, jalv.uis) {
			const LilvUI* this_ui = lilv_uis_get(jalv.uis, u);
			if (lilv_ui_is_supported(this_ui,
			                         suil_ui_supported,
			                         native_ui_type,
			                         &jalv.ui_type)) {
				/* TODO: Multiple UI support */
				jalv.ui = this_ui;
				break;
			}
		}
	} else if (!jalv.opts.generic_ui && jalv.opts.show_ui) {
		jalv.ui = lilv_uis_get(jalv.uis, lilv_uis_begin(jalv.uis));
	}

	/* Create ringbuffers for UI if necessary */
	if (jalv.ui) {
		fprintf(stderr, "UI:           %s\n",
		        lilv_node_as_uri(lilv_ui_get_uri(jalv.ui)));
	} else {
		fprintf(stderr, "UI:           None\n");
	}

	/* Create port structures (jalv.ports) */
	jalv_create_ports(&jalv);

	/* Determine the name of the JACK client */
	char* jack_name = NULL;
	if (jalv.opts.name) {
		/* Name given on command line */
		jack_name = jalv_strdup(jalv.opts.name);
	} else {
		/* Use plugin name */
		LilvNode* name = lilv_plugin_get_name(jalv.plugin);
		jack_name = jalv_strdup(lilv_node_as_string(name));
		lilv_node_free(name);
	}

	/* Truncate client name to suit JACK if necessary */
	if (strlen(jack_name) >= (unsigned)jack_client_name_size() - 1) {
		jack_name[jack_client_name_size() - 1] = '\0';
	}

	/* Connect to JACK */
	printf("JACK Name:    %s\n", jack_name);
#ifdef JALV_JACK_SESSION
	if (jalv.opts.uuid) {
		jalv.jack_client = jack_client_open(
			jack_name,
			JackSessionID | (jalv.opts.name_exact ? JackUseExactName : 0),
			NULL,
			jalv.opts.uuid);
	}
#endif

	if (!jalv.jack_client) {
		jalv.jack_client = jack_client_open(
			jack_name,
			(jalv.opts.name_exact ? JackUseExactName : JackNullOption),
			NULL);
	}

	free(jack_name);

	if (!jalv.jack_client)
		die("Failed to connect to JACK.\n");

	jalv.sample_rate  = jack_get_sample_rate(jalv.jack_client);
	jalv.block_length = jack_get_buffer_size(jalv.jack_client);
#ifdef HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE
	jalv.midi_buf_size = jack_port_type_get_buffer_size(
		jalv.jack_client, JACK_DEFAULT_MIDI_TYPE);
#else
	jalv.midi_buf_size = 4096;
	fprintf(stderr, "warning: No jack_port_type_get_buffer_size.\n");
#endif
	printf("Block length: %u frames\n", jalv.block_length);
	printf("MIDI buffers: %zu bytes\n", jalv.midi_buf_size);

	if (jalv.opts.buffer_size == 0) {
		/* The UI ring is fed by plugin output ports (usually one), and the UI
		   updates roughly once per cycle.  The ring size is a few times the
		   size of the MIDI output to give the UI a chance to keep up.  The UI
		   should be able to keep up with 4 cycles, and tests show this works
		   for me, but this value might need increasing to avoid overflows.
		*/
		jalv.opts.buffer_size = jalv.midi_buf_size * N_BUFFER_CYCLES;
	}

	if (jalv.opts.update_rate == 0.0) {
		/* Calculate a reasonable UI update frequency. */
		jalv.ui_update_hz = (float)jalv.sample_rate / jalv.midi_buf_size * 2.0f;
		jalv.ui_update_hz = MAX(25.0f, jalv.ui_update_hz);
	} else {
		/* Use user-specified UI update rate. */
		jalv.ui_update_hz = jalv.opts.update_rate;
		jalv.ui_update_hz = MAX(1.0f, jalv.ui_update_hz);
	}

	/* The UI can only go so fast, clamp to reasonable limits */
	jalv.ui_update_hz     = MIN(60, jalv.ui_update_hz);
	jalv.opts.buffer_size = MAX(4096, jalv.opts.buffer_size);
	fprintf(stderr, "Comm buffers: %d bytes\n", jalv.opts.buffer_size);
	fprintf(stderr, "Update rate:  %.01f Hz\n", jalv.ui_update_hz);

	/* Build options array to pass to plugin */
	const LV2_Options_Option options[] = {
		{ LV2_OPTIONS_INSTANCE, 0, jalv.urids.param_sampleRate,
		  sizeof(float), jalv.urids.atom_Float, &jalv.sample_rate },
		{ LV2_OPTIONS_INSTANCE, 0, jalv.urids.bufsz_minBlockLength,
		  sizeof(int32_t), jalv.urids.atom_Int, &jalv.block_length },
		{ LV2_OPTIONS_INSTANCE, 0, jalv.urids.bufsz_maxBlockLength,
		  sizeof(int32_t), jalv.urids.atom_Int, &jalv.block_length },
		{ LV2_OPTIONS_INSTANCE, 0, jalv.urids.bufsz_sequenceSize,
		  sizeof(int32_t), jalv.urids.atom_Int, &jalv.midi_buf_size },
		{ LV2_OPTIONS_INSTANCE, 0, jalv.urids.ui_updateRate,
		  sizeof(float), jalv.urids.atom_Float, &jalv.ui_update_hz },
		{ LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL }
	};

	options_feature.data = &options;

	/* Create Plugin <=> UI communication buffers */
	jalv.ui_events     = jack_ringbuffer_create(jalv.opts.buffer_size);
	jalv.plugin_events = jack_ringbuffer_create(jalv.opts.buffer_size);
	jack_ringbuffer_mlock(jalv.ui_events);
	jack_ringbuffer_mlock(jalv.plugin_events);

	/* Instantiate the plugin */
	jalv.instance = lilv_plugin_instantiate(
		jalv.plugin, jalv.sample_rate, features);
	if (!jalv.instance) {
		die("Failed to instantiate plugin.\n");
	}

	ext_data.data_access = lilv_instance_get_descriptor(jalv.instance)->extension_data;

	fprintf(stderr, "\n");
	if (!jalv.buf_size_set) {
		jalv_allocate_port_buffers(&jalv);
	}

	/* Create thread and ringbuffers for worker if necessary */
	if (lilv_plugin_has_feature(jalv.plugin, jalv.nodes.work_schedule)
	    && lilv_plugin_has_extension_data(jalv.plugin, jalv.nodes.work_interface)) {
		jalv_worker_init(
			&jalv, &jalv.worker,
			(const LV2_Worker_Interface*)lilv_instance_get_extension_data(
				jalv.instance, LV2_WORKER__interface));
	}

	/* Apply loaded state to plugin instance if necessary */
	if (state) {
		jalv_apply_state(&jalv, state);
	}

	if (jalv.opts.controls) {
		for (char** c = jalv.opts.controls; *c; ++c) {
			jalv_apply_control_arg(&jalv, *c);
		}
	}

	/* Set Jack callbacks */
	jack_set_process_callback(jalv.jack_client,
	                          &jack_process_cb, (void*)(&jalv));
	jack_set_buffer_size_callback(jalv.jack_client,
	                              &jack_buffer_size_cb, (void*)(&jalv));
	jack_on_shutdown(jalv.jack_client,
	                 &jack_shutdown_cb, (void*)(&jalv));
	jack_set_latency_callback(jalv.jack_client,
	                          &jack_latency_cb, (void*)(&jalv));
#ifdef JALV_JACK_SESSION
	jack_set_session_callback(jalv.jack_client,
	                          &jack_session_cb, (void*)(&jalv));
#endif

	/* Create Jack ports and connect plugin ports to buffers */
	for (uint32_t i = 0; i < jalv.num_ports; ++i) {
		activate_port(&jalv, i);
	}

	/* Activate plugin */
	lilv_instance_activate(jalv.instance);

	/* Activate Jack */
	jack_activate(jalv.jack_client);
	jalv.sample_rate = jack_get_sample_rate(jalv.jack_client);
	jalv.play_state  = JALV_RUNNING;

	/* Run UI (or prompt at console) */
	jalv_open_ui(&jalv);

	/* Wait for finish signal from UI or signal handler */
	zix_sem_wait(&exit_sem);
	jalv.exit = true;

	fprintf(stderr, "Exiting...\n");

	/* Terminate the worker */
	jalv_worker_finish(&jalv.worker);

	/* Deactivate JACK */
	jack_deactivate(jalv.jack_client);
	for (uint32_t i = 0; i < jalv.num_ports; ++i) {
		if (jalv.ports[i].evbuf) {
			lv2_evbuf_free(jalv.ports[i].evbuf);
		}
	}
	jack_client_close(jalv.jack_client);

	/* Deactivate plugin */
	suil_instance_free(jalv.ui_instance);
	lilv_instance_deactivate(jalv.instance);
	lilv_instance_free(jalv.instance);

	/* Clean up */
	free(jalv.ports);
	jack_ringbuffer_free(jalv.ui_events);
	jack_ringbuffer_free(jalv.plugin_events);
	for (LilvNode** n = (LilvNode**)&jalv.nodes; *n; ++n) {
		lilv_node_free(*n);
	}
	symap_free(jalv.symap);
	zix_sem_destroy(&jalv.symap_lock);
	suil_host_free(jalv.ui_host);
	sratom_free(jalv.sratom);
	sratom_free(jalv.ui_sratom);
	lilv_uis_free(jalv.uis);
	lilv_world_free(world);

	zix_sem_destroy(&exit_sem);

	remove(jalv.temp_dir);
	free(jalv.temp_dir);
	free(jalv.ui_event_buf);

	return 0;
}
