/*
 * Copyright (c) 2016 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <peer.h>
#include <stdio.h>
#include <string.h>

#include "pico_clip_core1_test_shared.h"

static struct net_mgmt_event_callback net_cb;
static K_SEM_DEFINE(ipv4_ready_sem, 0, 1);
static bool wifi_ready;
static bool wifi_connecting;
static bool peer_running;
static bool peer_stop;
static PeerConnection *g_pc;
static PeerConnectionState g_state = PEER_CONNECTION_NEW;
static uint32_t g_last_audio_seq;
static uint32_t g_audio_sent;
static uint32_t g_audio_silence_sent;
static uint32_t g_audio_send_failed;
static uint32_t g_audio_dropped;
static uint32_t g_remote_audio_seq;
static uint32_t g_remote_audio_dropped;
static char g_url[192];
static char g_token[128];
static const char g_wifi_ssid[] = "yu_2.4G";
static const char g_wifi_psk[] = "12120905";

static void wifi_reconnect_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(wifi_reconnect_work, wifi_reconnect_work_handler);

K_THREAD_STACK_DEFINE(peer_thread_stack, 6144);
K_THREAD_STACK_DEFINE(signaling_thread_stack, 6144);
static struct k_thread peer_thread;
static struct k_thread signaling_thread;
K_SEM_DEFINE(signaling_done_sem, 0, 1);
K_MUTEX_DEFINE(peer_mutex);
static int peer_test_start(const char *url, const char *token);
static void peer_lock(void);
static void peer_unlock(void);

static void webrtc_test_usage(const struct shell *sh)
{
	shell_print(sh,
		    "Usage: webrtc_test start -u <url> [-t <token>] | stop | status");
}

static void wifi_schedule_reconnect(void)
{
	wifi_ready = false;
	wifi_connecting = false;
	printk("Wi-Fi reconnect in 5 seconds\n");
	(void)k_work_reschedule(&wifi_reconnect_work, K_SECONDS(5));
}

static bool wifi_has_ipv4(void)
{
	struct net_if *iface = net_if_get_default();
	struct in_addr *ifaddr;

	if (iface == NULL) {
		return false;
	}

	ifaddr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (ifaddr == NULL) {
		ifaddr = net_if_ipv4_get_global_addr(iface, NET_ADDR_DHCP);
	}

	return ifaddr != NULL;
}

static int wifi_auto_connect(void)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_connect_req_params params = { 0 };
	int ret;

	if (iface == NULL) {
		printk("Wi-Fi iface not ready\n");
		return -ENODEV;
	}

	params.ssid = (const uint8_t *)g_wifi_ssid;
	params.ssid_length = strlen(g_wifi_ssid);
	params.psk = (const uint8_t *)g_wifi_psk;
	params.psk_length = strlen(g_wifi_psk);
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	params.mfp = WIFI_MFP_OPTIONAL;

	wifi_connecting = true;
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (ret == -EALREADY || ret == -EINPROGRESS) {
		printk("Wi-Fi connect already in progress\n");
		return 0;
	}

	if (ret < 0) {
		printk("Wi-Fi connect request failed: %d\n", ret);
		wifi_schedule_reconnect();
		return ret;
	}

	printk("Wi-Fi connect requested: %s\n", g_wifi_ssid);
	return 0;
}

static void wifi_reconnect_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (wifi_ready || wifi_connecting) {
		return;
	}

	(void)wifi_auto_connect();
}

static void on_net_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			 struct net_if *iface)
{
	struct in_addr *ifaddr;
	char addr_buf[NET_IPV4_ADDR_LEN];

	if (iface == NULL) {
		return;
	}

	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = cb->info;
		int st = 0;

		if (status != NULL && cb->info_length >= sizeof(*status)) {
			st = status->status;
		}

		wifi_connecting = false;
		if (st != 0) {
			printk("Wi-Fi connect failed: %d\n", st);
			wifi_schedule_reconnect();
		} else {
			printk("Wi-Fi connected, waiting for IPv4\n");
		}
		return;
	}

	if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		const struct wifi_status *status = cb->info;
		int st = 0;

		if (status != NULL && cb->info_length >= sizeof(*status)) {
			st = status->status;
		}

		printk("Wi-Fi disconnected: %d\n", st);
		wifi_schedule_reconnect();
		return;
	}

	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		ifaddr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (ifaddr == NULL) {
			ifaddr = net_if_ipv4_get_global_addr(iface, NET_ADDR_DHCP);
		}
		if (ifaddr == NULL) {
			printk("IPv4 event received, address not ready yet\n");
		} else {
			net_addr_ntop(AF_INET, ifaddr, addr_buf, sizeof(addr_buf));
			printk("IPv4 ready: %s\n", addr_buf);
			wifi_ready = true;
			k_sem_give(&ipv4_ready_sem);
		}
	}
}

static int webrtc_test_start_cmd(const struct shell *sh, size_t argc, char **argv)
{
	const char *url;
	const char *token = "";
	int ret;

	if (argc != 3 && argc != 5) {
		webrtc_test_usage(sh);
		return -EINVAL;
	}

	if (strcmp(argv[1], "-u") != 0) {
		webrtc_test_usage(sh);
		return -EINVAL;
	}

	if (argc == 5) {
		if (strcmp(argv[3], "-t") != 0) {
			webrtc_test_usage(sh);
			return -EINVAL;
		}
		token = argv[4];
	}

	if (!wifi_ready && wifi_has_ipv4()) {
		wifi_ready = true;
	}

	if (!wifi_ready) {
		shell_error(sh, "Wi-Fi not ready. Run wifi connect and wait for IPv4.");
		return -EAGAIN;
	}

	if (peer_running) {
		shell_error(sh, "peer test already running");
		return -EALREADY;
	}

	url = argv[2];
	if ((pico_clip_core1_test_shm()->audio_flags &
	     PICO_CLIP_CORE1_AUDIO_FLAG_OPUS_READY) == 0U) {
		shell_print(sh, "warning: core1 audio stream is not ready");
	}
	shell_print(sh, "starting WebRTC PCMU audio test, url=%s", url);

	ret = peer_test_start(url, token);
	if (ret < 0) {
		shell_error(sh, "peer_test_start failed: %d", ret);
		return ret;
	}

	shell_print(sh, "peer test started");
	return 0;
}

static int cmd_webrtc_test(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		webrtc_test_usage(sh);
		return -EINVAL;
	}

	if (strcmp(argv[1], "start") == 0) {
		return webrtc_test_start_cmd(sh, argc - 1, &argv[1]);
	}

	if (strcmp(argv[1], "stop") == 0) {
		if (!peer_running) {
			shell_print(sh, "WebRTC test is not running");
			return 0;
		}
		peer_stop = true;
		if (g_pc != NULL) {
			peer_connection_close(g_pc);
		}
		shell_print(sh, "WebRTC test stopping");
		return 0;
	}

	if (strcmp(argv[1], "status") == 0) {
		volatile struct pico_clip_core1_test_shared *shared =
			pico_clip_core1_test_shm();

		shell_print(sh, "webrtc: running=%u state=%s",
			    peer_running,
			    g_pc != NULL ? peer_connection_state_to_string(g_state) : "none");
		shell_print(sh,
			    "pcmu tx: sent=%u silence=%u dropped=%u fail=%u last_seq=%u core1_seq=%u len=%u flags=0x%x",
			    g_audio_sent, g_audio_silence_sent, g_audio_dropped, g_audio_send_failed,
			    g_last_audio_seq, shared->opus_seq, shared->opus_len,
			    shared->audio_flags);
		shell_print(sh, "pcmu rx: queued=%u dropped=%u pending=%u",
			    g_remote_audio_seq, g_remote_audio_dropped,
			    shared->spk_opus_write_seq - shared->spk_opus_read_seq);
		return 0;
	}

	webrtc_test_usage(sh);
	return -EINVAL;
}

static int cmd_peer_test(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "-u") == 0) {
		return webrtc_test_start_cmd(sh, argc, argv);
	}

	return cmd_webrtc_test(sh, argc, argv);
}

SHELL_CMD_REGISTER(webrtc_test, NULL, "Run WebRTC PCMU audio test",
		   cmd_webrtc_test);
SHELL_CMD_REGISTER(peer_test, NULL, "Alias for webrtc_test", cmd_peer_test);

static int cmd_cpu_freq(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t start;
	uint32_t end;
	uint32_t delta;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	start = k_cycle_get_32();
	k_busy_wait(USEC_PER_SEC);
	end = k_cycle_get_32();
	delta = end - start;

	shell_print(sh, "cycles/sec: %u", delta);
	shell_print(sh, "cpu: %u.%03u MHz", delta / 1000000U, (delta % 1000000U) / 1000U);

	return 0;
}

SHELL_CMD_REGISTER(cpu_freq, NULL, "Measure CPU frequency over 1 second.", cmd_cpu_freq);

static void onconnectionstatechange(PeerConnectionState state, void *data)
{
	volatile struct pico_clip_core1_test_shared *shared = pico_clip_core1_test_shm();
	uint32_t seq;

	ARG_UNUSED(data);
	g_state = state;
	printk("peer state: %s\n", peer_connection_state_to_string(state));

	if (state == PEER_CONNECTION_CONNECTED &&
	    shared->magic == PICO_CLIP_CORE1_TEST_MAGIC &&
	    shared->version == PICO_CLIP_CORE1_TEST_VERSION &&
	    (shared->audio_flags & PICO_CLIP_CORE1_AUDIO_FLAG_OPUS_READY) == 0U) {
		seq = shared->cpu0_audio_cmd_seq + 1U;
		shared->cpu0_audio_cmd = PICO_CLIP_CORE1_AUDIO_CMD_OPUS;
		shared->cpu0_audio_cmd_seq = seq;
		__SEV();
		printk("core1 20 ms double-buffered microphone Opus stream requested seq=%u\n",
		       seq);
	}
}

static void onaudiotrack(uint8_t *data, size_t size, void *user_data)
{
	ARG_UNUSED(user_data);
	volatile struct pico_clip_core1_test_shared *shared = pico_clip_core1_test_shm();
	uint32_t write_seq;
	uint32_t read_seq;
	uint32_t slot;

	g_remote_audio_seq++;
	if (shared->magic != PICO_CLIP_CORE1_TEST_MAGIC ||
	    shared->version != PICO_CLIP_CORE1_TEST_VERSION ||
	    (shared->audio_flags & PICO_CLIP_CORE1_AUDIO_FLAG_SPK_READY) == 0U ||
	    size == 0U || size > PICO_CLIP_CORE1_OPUS_MAX_PACKET) {
		g_remote_audio_dropped++;
		return;
	}

	write_seq = shared->spk_opus_write_seq;
	read_seq = shared->spk_opus_read_seq;
	if ((write_seq - read_seq) >= PICO_CLIP_CORE1_SPK_OPUS_QUEUE) {
		shared->spk_opus_dropped++;
		g_remote_audio_dropped++;
		return;
	}

	slot = write_seq % PICO_CLIP_CORE1_SPK_OPUS_QUEUE;
	shared->spk_opus_slot_seq[slot] = 0;
	memcpy((void *)shared->spk_opus_packet[slot], data, size);
	shared->spk_opus_len[slot] = (uint32_t)size;
	__DMB();
	shared->spk_opus_slot_seq[slot] = write_seq + 1U;
	shared->spk_opus_write_seq = write_seq + 1U;
	if (write_seq + 1U - read_seq > shared->spk_opus_pending_max) {
		shared->spk_opus_pending_max = write_seq + 1U - read_seq;
	}
	__SEV();

	if ((g_remote_audio_seq % 50U) == 0U) {
		printk("remote pcmu queued=%u dropped=%u pending=%u\n",
		       g_remote_audio_seq, g_remote_audio_dropped,
		       shared->spk_opus_write_seq - shared->spk_opus_read_seq);
	}
}

static void peer_lock(void)
{
	k_mutex_lock(&peer_mutex, K_FOREVER);
}

static void peer_unlock(void)
{
	k_mutex_unlock(&peer_mutex);
}

static void peer_send_core1_audio(void)
{
	volatile struct pico_clip_core1_test_shared *shared = pico_clip_core1_test_shm();
	uint8_t packet[PICO_CLIP_CORE1_OPUS_MAX_PACKET];
	uint32_t seq;
	uint32_t seq_after;
	uint32_t len;
	bool silence;
	int ret;

	if (g_pc == NULL || g_state != PEER_CONNECTION_CONNECTED) {
		return;
	}

	if (shared->magic != PICO_CLIP_CORE1_TEST_MAGIC ||
	    shared->version != PICO_CLIP_CORE1_TEST_VERSION ||
	    (shared->audio_flags & PICO_CLIP_CORE1_AUDIO_FLAG_OPUS_READY) == 0U) {
		return;
	}

	seq = shared->opus_seq;
	if (seq == 0U || seq == g_last_audio_seq) {
		return;
	}

	len = shared->opus_len;
	if (len == 0U || len > sizeof(packet)) {
		g_last_audio_seq = seq;
		g_audio_dropped++;
		return;
	}

	memcpy(packet, (const void *)shared->opus_packet, len);
	silence = shared->opus_silence != 0u;
	seq_after = shared->opus_seq;
	if (seq_after != seq || seq_after == 0U) {
		return;
	}

	ret = peer_connection_send_audio(g_pc, packet, len);
	if (ret < 0) {
		g_audio_send_failed++;
		return;
	}

	if (seq > g_last_audio_seq + 1U && g_last_audio_seq != 0U) {
		g_audio_dropped += seq - g_last_audio_seq - 1U;
	}
	g_last_audio_seq = seq;
	g_audio_sent++;
	if (silence) {
		g_audio_silence_sent++;
		printk("pcmu silence heartbeat sent=%u seq=%u len=%u\n",
		       g_audio_silence_sent, seq, len);
	}
	if ((g_audio_sent % 50U) == 0U) {
		printk("pcmu audio sent=%u dropped=%u fail=%u seq=%u len=%u\n",
		       g_audio_sent, g_audio_dropped, g_audio_send_failed, seq, len);
	}
}

static void signaling_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	printk("signaling thread start\n");
	while (!peer_stop) {
		(void)peer_signaling_loop();
		k_msleep(1);
	}
	printk("signaling thread stop\n");
	k_sem_give(&signaling_done_sem);
}

static void peer_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	printk("peer connection thread start\n");
	while (!peer_stop) {
		peer_lock();
		(void)peer_connection_loop(g_pc);
		peer_send_core1_audio();
		peer_unlock();
		if (g_state == PEER_CONNECTION_FAILED || g_state == PEER_CONNECTION_CLOSED) {
			printk("peer loop exit on state=%s\n", peer_connection_state_to_string(g_state));
			break;
		}
		if (g_state == PEER_CONNECTION_CONNECTED) {
			k_yield();
		} else {
			k_msleep(1);
		}
	}

	peer_stop = true;
	(void)k_sem_take(&signaling_done_sem, K_SECONDS(2));
	peer_signaling_disconnect();
	peer_connection_destroy(g_pc);
	peer_deinit();
	g_pc = NULL;
	peer_running = false;
	printk("peer stopped\n");
}

static int peer_test_start(const char *url, const char *token)
{
	PeerConfiguration config = {
		.ice_servers = {
			{ .urls = "stun:stun.l.google.com:19302" },
		},
		.datachannel = DATA_CHANNEL_NONE,
		.video_codec = CODEC_NONE,
		.audio_codec = CODEC_PCMU,
		.onaudiotrack = onaudiotrack,
	};

	snprintf(g_url, sizeof(g_url), "%s", url);
	snprintf(g_token, sizeof(g_token), "%s", token ? token : "");

	peer_init();
	g_pc = peer_connection_create(&config);
	if (!g_pc) {
		printk("peer_connection_create failed\n");
		peer_deinit();
		return -ENOMEM;
	}

	peer_connection_oniceconnectionstatechange(g_pc, onconnectionstatechange);
	peer_signaling_set_peer_lock(peer_lock, peer_unlock);

	if (peer_signaling_connect(g_url, g_token, g_pc) < 0) {
		printk("peer_signaling_connect failed\n");
		peer_connection_destroy(g_pc);
		g_pc = NULL;
		peer_deinit();
		return -EIO;
	}

	peer_stop = false;
	peer_running = true;
	g_last_audio_seq = 0;
	g_audio_sent = 0;
	g_audio_silence_sent = 0;
	g_audio_send_failed = 0;
	g_audio_dropped = 0;
	g_remote_audio_seq = 0;
	g_remote_audio_dropped = 0;
	k_sem_reset(&signaling_done_sem);
	k_thread_create(&signaling_thread, signaling_thread_stack,
			K_THREAD_STACK_SIZEOF(signaling_thread_stack),
			signaling_worker, NULL, NULL, NULL, K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
	k_thread_create(&peer_thread, peer_thread_stack, K_THREAD_STACK_SIZEOF(peer_thread_stack),
			peer_worker, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	return 0;
}

int main(void)
{
	printk("wifi_shell app start\n");
	printk("Auto Wi-Fi connect enabled: SSID=%s\n", g_wifi_ssid);

	net_mgmt_init_event_callback(&net_cb, on_net_event,
				     NET_EVENT_IPV4_ADDR_ADD |
					     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&net_cb);
	(void)wifi_auto_connect();

	printk("waiting for IPv4 address...\n");
	k_sem_take(&ipv4_ready_sem, K_FOREVER);
	printk("network is ready, you can run: peer_test -u <url>\n");

	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
