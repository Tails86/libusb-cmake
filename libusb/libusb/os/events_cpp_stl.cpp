/*
 * windows hotplug backend for libusb 1.0
 * Copyright © 2025 James Smith <jmsmith86@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libusbi.h"
#include "events_cpp_stl.h"

#include <cstdlib>
#include <chrono>
#include <mutex>
#include <condition_variable>

#ifndef HAVE_CLOCK_GETTIME
void usbi_get_monotonic_time(struct timespec *tp)
{
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration duration = now.time_since_epoch();
    std::chrono::seconds seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    std::chrono::nanoseconds nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
    tp->tv_sec = seconds.count();
    tp->tv_nsec = nanoseconds.count();
}
#endif

static std::mutex event_mutex;
static std::condition_variable event_cv;

struct cpp_stl_usbi_event
{
    int event_occurred = 0;
};

int usbi_create_event(usbi_event_t *event)
{
    (*event) = new cpp_stl_usbi_event();
    return 0;
}

void usbi_destroy_event(usbi_event_t *event)
{
    delete (*event);
    (*event) = nullptr;
}

void usbi_signal_event(usbi_event_t *event)
{
    std::unique_lock<std::mutex> lock(event_mutex);
    (*event)->event_occurred = 1;
    event_cv.notify_all();
}

void usbi_clear_event(usbi_event_t *event)
{
    std::unique_lock<std::mutex> lock(event_mutex);
    (*event)->event_occurred = 0;
}

#ifdef HAVE_OS_TIMER
struct cpp_stl_usbi_timer
{
    bool armed = false;
    std::chrono::steady_clock::time_point time;
};

int usbi_timer_valid(usbi_timer_t *timer)
{
	return (*timer) != nullptr;
}

int usbi_create_timer(usbi_timer_t *timer)
{
	(*timer) = new cpp_stl_usbi_timer();

	return 0;
}

void usbi_destroy_timer(usbi_timer_t *timer)
{
	delete (*timer);
    (*timer) = nullptr;
}

int usbi_arm_timer(usbi_timer_t *timer, const struct timespec *timeout)
{
    (*timer)->time =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(timeout->tv_sec) +
        std::chrono::nanoseconds(timeout->tv_nsec);
    (*timer)->armed = true;
    return 0;
}

int usbi_disarm_timer(usbi_timer_t *timer)
{
	(*timer)->armed = false;

	return 0;
}
#endif // HAVE_OS_TIMER

int usbi_alloc_event_data(struct libusb_context *ctx)
{
	struct usbi_event_source *ievent_source;
	void **handles;
	size_t i = 0;

	/* Event sources are only added during usbi_io_init(). We should not
	 * be running this function again if the event data has already been
	 * allocated. */
	if (ctx->event_data) {
		usbi_warn(ctx, "program assertion failed - event data already allocated");
		return LIBUSB_ERROR_OTHER;
	}

	ctx->event_data_cnt = 0;
	for_each_event_source(ctx, ievent_source)
		ctx->event_data_cnt++;

	/* We only expect up to two HANDLEs to wait on, one for the internal
	 * signalling event and the other for the timer. */
	if (ctx->event_data_cnt != 1 && ctx->event_data_cnt != 2) {
		usbi_err(ctx, "program assertion failed - expected exactly 1 or 2 HANDLEs");
		return LIBUSB_ERROR_OTHER;
	}

	handles = static_cast<void**>(calloc(ctx->event_data_cnt, sizeof(void*)));
	if (!handles)
		return LIBUSB_ERROR_NO_MEM;

	for_each_event_source(ctx, ievent_source) {
		handles[i] = ievent_source->data.os_handle;
		i++;
	}

	ctx->event_data = handles;
	return 0;
}

int usbi_wait_for_events(struct libusb_context *ctx, struct usbi_reported_events *reported_events, int timeout_ms)
{
	void **handles = static_cast<void**>(ctx->event_data);
	int num_handles = ctx->event_data_cnt;

	usbi_dbg(ctx, "wait for %lu HANDLEs with timeout in %dms", static_cast<unsigned long>(num_handles), timeout_ms);

    reported_events->num_ready = 0;
    reported_events->event_triggered = 0;

#ifdef HAVE_OS_TIMER
    reported_events->timer_triggered = 0;
    bool timerElapsedOnTimeout = false;
    if (usbi_using_timer(ctx))
    {
        if (num_handles > 1)
        {
            cpp_stl_usbi_timer *tmr = static_cast<cpp_stl_usbi_timer*>(handles[1]);
            if (tmr->armed)
            {
                auto now = std::chrono::steady_clock::now();
                std::chrono::milliseconds tmrTimeoutMs(0);
                if (tmr->time > now)
                {
                    tmrTimeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(tmr->time - now);
                }
                if (tmrTimeoutMs.count() <= timeout_ms)
                {
                    timerElapsedOnTimeout = true;
                    timeout_ms = tmrTimeoutMs.count();
                }
            }
        }
    }
#endif

    if (num_handles > 0)
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        bool status = event_cv.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            [&handles, &num_handles, &reported_events]()
            {
                bool trigger = false;
                cpp_stl_usbi_event *ev = static_cast<cpp_stl_usbi_event*>(handles[0]);

                if (ev->event_occurred)
                {
                    trigger = true;
                    reported_events->event_triggered = 1;
                    ++reported_events->num_ready;
                }

                return trigger;
            }
        );

#if HAVE_OS_TIMER
        if (!status && timerElapsedOnTimeout)
        {
            reported_events->timer_triggered = 1;
            ++reported_events->num_ready;
            status = true;
        }
        else if (status && usbi_using_timer(ctx))
        {
            cpp_stl_usbi_timer *tmr = static_cast<cpp_stl_usbi_timer*>(handles[1]);
            if (tmr->armed)
            {
                auto now = std::chrono::steady_clock::now();
                if (now >= tmr->time)
                {
                    reported_events->timer_triggered = 1;
                    ++reported_events->num_ready;
                }
            }
        }
#endif

        if (!status)
        {
            return LIBUSB_ERROR_TIMEOUT;
        }
    }

	return LIBUSB_SUCCESS;
}
