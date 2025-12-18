/*
 * Copyright (c) 2024 Vogl Electronic GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/hawkbit/hawkbit.h>
#include <zephyr/mgmt/hawkbit/config.h>
#include <zephyr/mgmt/hawkbit/autohandler.h>

LOG_MODULE_DECLARE(hawkbit);

static void autohandler(struct k_work *work);

#if IS_ENABLED(CONFIG_HAWKBIT_AUTOHANDLER_WORKQUEUE)
struct k_work_q *handler_work_q = &k_sys_work_q;
#else
K_THREAD_STACK_DEFINE(hawkbit_work_stack, CONFIG_HAWKBIT_AUTOHANDLER_WORKQUEUE_STACK_SIZE);
struct k_work_q hawkbit_work_q;
static const struct k_work_queue_config hawkbit_work_q_config = {
	.name = "hawkbit_work_q", .no_yield = false, .essential = false, 0};

struct k_work_q *handler_work_q = &hawkbit_work_q;
#endif

static K_WORK_DELAYABLE_DEFINE(hawkbit_work_handle, autohandler);
static K_WORK_DELAYABLE_DEFINE(hawkbit_work_handle_once, autohandler);

static K_EVENT_DEFINE(hawkbit_autohandler_event);

static void autohandler(struct k_work *work)
{
	k_event_clear(&hawkbit_autohandler_event, UINT32_MAX);

	enum hawkbit_response response = hawkbit_probe();

	k_event_set(&hawkbit_autohandler_event, BIT(response));

	switch (response) {
	case HAWKBIT_UNCONFIRMED_IMAGE:
		LOG_ERR("Current image is not confirmed");
		LOG_ERR("Rebooting to previous confirmed image");
		LOG_ERR("If this image is flashed using a hardware tool");
		LOG_ERR("Make sure that it is a confirmed image");
		hawkbit_reboot();
		break;

	case HAWKBIT_NO_UPDATE:
		LOG_INF("No update found");
		break;

	case HAWKBIT_UPDATE_INSTALLED:
		LOG_INF("Update installed");
		hawkbit_reboot();
		break;

	case HAWKBIT_ALLOC_ERROR:
		LOG_INF("Memory allocation error");
		break;

	case HAWKBIT_DOWNLOAD_ERROR:
		LOG_INF("Update failed");
		break;

	case HAWKBIT_NETWORKING_ERROR:
		LOG_INF("Network error");
		break;

	case HAWKBIT_PERMISSION_ERROR:
		LOG_INF("Permission error");
		break;

	case HAWKBIT_METADATA_ERROR:
		LOG_INF("Metadata error");
		break;

	case HAWKBIT_NOT_INITIALIZED:
		LOG_INF("hawkBit not initialized");
		break;

	case HAWKBIT_PROBE_IN_PROGRESS:
		LOG_INF("hawkBit is already running");
		break;

	default:
		LOG_ERR("Invalid response: %d", response);
		break;
	}

	if (k_work_delayable_from_work(work) == &hawkbit_work_handle) {
		k_work_reschedule_for_queue(handler_work_q, &hawkbit_work_handle,
					    K_SECONDS(hawkbit_get_poll_interval()));
	}
}

enum hawkbit_response hawkbit_autohandler_wait(uint32_t events, k_timeout_t timeout)
{
	uint32_t ret = k_event_wait(&hawkbit_autohandler_event, events, false, timeout);

	for (int i = 1; i < HAWKBIT_PROBE_IN_PROGRESS; i++) {
		if (ret & BIT(i)) {
			return i;
		}
	}
	return HAWKBIT_NO_RESPONSE;
}

int hawkbit_autohandler_cancel(void)
{
	return k_work_cancel_delayable(&hawkbit_work_handle);
}

int hawkbit_autohandler_set_delay(k_timeout_t timeout, bool if_bigger)
{
	if (!if_bigger || timeout.ticks > k_work_delayable_remaining_get(&hawkbit_work_handle)) {
		hawkbit_autohandler_cancel();
		LOG_INF("Setting new delay for next run: %02u:%02u:%02u",
			(uint32_t)(timeout.ticks / CONFIG_SYS_CLOCK_TICKS_PER_SEC) / 3600,
			(uint32_t)((timeout.ticks / CONFIG_SYS_CLOCK_TICKS_PER_SEC) % 3600) / 60,
			(uint32_t)(timeout.ticks / CONFIG_SYS_CLOCK_TICKS_PER_SEC) % 60);
		return k_work_reschedule_for_queue(handler_work_q, &hawkbit_work_handle, timeout);
	}
	return 0;
}

void hawkbit_autohandler(bool auto_reschedule)
{
#if !IS_ENABLED(CONFIG_HAWKBIT_AUTOHANDLER_WORKQUEUE)
	static bool work_initialized = false;
	if (!work_initialized) {
		LOG_INF("Initializing hawkbit autohandler workqueue");
		k_work_queue_init(handler_work_q);
		k_work_queue_start(handler_work_q, hawkbit_work_stack,
				   K_THREAD_STACK_SIZEOF(hawkbit_work_stack),
				   K_PRIO_PREEMPT(CONFIG_HAWKBIT_AUTOHANDLER_WORKQUEUE_PRIORITY),
				   &hawkbit_work_q_config);
		work_initialized = true;
	}
#endif
	k_event_clear(&hawkbit_autohandler_event, UINT32_MAX);

	if (auto_reschedule) {
		k_work_reschedule_for_queue(handler_work_q, &hawkbit_work_handle, K_NO_WAIT);
	} else {
		k_work_reschedule_for_queue(handler_work_q, &hawkbit_work_handle_once, K_NO_WAIT);
	}
}
