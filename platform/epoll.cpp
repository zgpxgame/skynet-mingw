
#include "epoll.h"

#include <Winsock2.h>

#include <conio.h>
#include <errno.h>
#include <map>
#include <vector>

struct fd_t
{
    int fd;
    struct epoll_event event;
	HANDLE wsa_event;

    fd_t() {
	}

	~fd_t() {
	}
};

struct cs_t
{
	CRITICAL_SECTION cs;

	void lock()
	{ EnterCriticalSection(&cs); }

	void unlock()
	{ LeaveCriticalSection(&cs); }

	cs_t()
	{ InitializeCriticalSection(&cs); }

	~cs_t()
	{ DeleteCriticalSection(&cs); }
};

struct lock_t {
	cs_t& cs;

	lock_t(cs_t& cs) : cs(cs)
	{ cs.lock(); }

	~lock_t()
	{ cs.unlock(); }
};

cs_t cs;
#define GUARD()\
	lock_t lock(cs);

typedef std::vector<fd_t> ep_internal;

int ep_next_id;
std::map<int, ep_internal> ep_data;

long get_wsa_mask(unsigned int epoll_events)
{
    long mask = 0;
    if(epoll_events & EPOLLIN)
        mask |= FD_READ;
    if(epoll_events & EPOLLOUT)
        mask |= FD_WRITE;
    if(epoll_events & EPOLLRDHUP)
        mask |= 0; // ??
    if(epoll_events & EPOLLPRI)
        mask |= 0; // ??
    if(epoll_events & EPOLLERR)
        mask |= 0; // ??
    if(epoll_events & EPOLLHUP)
        mask |= FD_CLOSE;
    if(epoll_events & EPOLLET)
        mask |= 0; // ??
    return mask;
}

unsigned int get_ep_mask(WSANETWORKEVENTS* wsa_events)
{
    unsigned int mask = 0;
    if(wsa_events->lNetworkEvents & FD_READ)
        mask |= EPOLLIN;
    if(wsa_events->lNetworkEvents & FD_WRITE)
        mask |= EPOLLOUT;
    if(wsa_events->lNetworkEvents & FD_CLOSE)
        mask |= EPOLLHUP;
    return mask;
}

int epoll_startup()
{
    ep_next_id = 0;

    WSADATA wsadata;
    return WSAStartup(MAKEWORD(2, 2), &wsadata);
}

/*
Errors:
    EINVAL
        size is not positive.
    ENFILE
        The system limit on the total number of open files has been reached.
    ENOMEM
        There was insufficient memory to create the kernel object.
*/
int epoll_create(int size)
{
	GUARD();
    // maintaining error condition for compatibility
    // however, this parameter is ignored.
    if(size < 0)
        return EINVAL;

    ++ep_next_id;

    // ran out of ids!  wrapped around.
    if(ep_next_id > (ep_next_id + 1))
        ep_next_id = 0;

    while(ep_next_id < (ep_next_id + 1)) {

        if(ep_data.find(ep_next_id) == ep_data.end())
            break;
        ++ep_next_id;
    }

    if(ep_next_id < 0) {
        // two billion fds, eh...
        return ENFILE;
    }

    ep_data[ep_next_id] = ep_internal();
    return ep_next_id;
}

/*
    EPOLL_CTL_ADD
        Add the target file descriptor fd to the epoll descriptor epfd and associate the event event with the internal file linked to fd.
    EPOLL_CTL_MOD
        Change the event event associated with the target file descriptor fd.
    EPOLL_CTL_DEL
        Remove the target file descriptor fd from the epoll file descriptor, epfd. The event is ignored and can be NULL (but see BUGS below).
Errors:
    EBADF
        epfd or fd is not a valid file descriptor.
    EEXIST
        op was EPOLL_CTL_ADD, and the supplied file descriptor fd is already in epfd.
    EINVAL
        epfd is not an epoll file descriptor, or fd is the same as epfd, or the requested operation op is not supported by this interface.
    ENOENT
        op was EPOLL_CTL_MOD or EPOLL_CTL_DEL, and fd is not in epfd.
    ENOMEM
        There was insufficient memory to handle the requested op control operation.
    EPERM
        The target file fd does not support epoll.
*/
int epoll_ctl(int epfd, int opcode, int fd, struct epoll_event* event)
{
	GUARD();
    if(epfd < 0 || ep_data.find(epfd) == ep_data.end())
        return EBADF;

    // TODO: find out if it's possible to tell whether fd is a socket
    // descriptor.  If so, make sure it is; if not, set EPERM and return -1.

    ep_internal& epi = ep_data[epfd];

    if(opcode == EPOLL_CTL_ADD) {

        for(ep_internal::size_type i = 0; i < epi.size(); ++i) { 
            if(epi[i].fd == fd)
                return EEXIST;
        }

        fd_t f;
		f.fd = fd;
		f.event = *event;
		f.wsa_event = WSACreateEvent();
        f.event.events |= EPOLLHUP;
        f.event.events |= EPOLLERR;
        WSAEventSelect(f.fd, f.wsa_event, FD_ACCEPT | FD_CLOSE | get_wsa_mask(f.event.events));
        epi.push_back(f);
		return 0;
    }
    else if(opcode == EPOLL_CTL_MOD) {

        for(ep_internal::size_type i = 0; i < epi.size(); ++i) {

            if(epi[i].fd == fd) {

                epi[i].event = *event;
                epi[i].event.events |= EPOLLHUP;
                epi[i].event.events |= EPOLLERR;
		        WSAEventSelect(epi[i].fd, epi[i].wsa_event, FD_ACCEPT | get_wsa_mask(epi[i].event.events));
                return 0;
            }
        }
        return ENOENT;
    }
    else if(opcode == EPOLL_CTL_DEL) {

		for(ep_internal::iterator itr = epi.begin(); itr != epi.end(); ++itr) {

			if(itr->fd == fd) {
				// now unset the event notifications
				WSAEventSelect(itr->fd, 0, 0);
				// clean up event
				WSACloseEvent(itr->wsa_event);
				epi.erase(itr);
				return 0;
			}
		}
        return ENOENT;
    }
    return EINVAL;
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout)
{
	GUARD();
    if(epfd < 0 || ep_data.find(epfd) == ep_data.end() || maxevents < 1)
        /* EINVAL */
        return -1;

    ep_internal& epi = ep_data[epfd];

    WSAEVENT* wsa_events = new WSAEVENT[epi.size()];
    for(ep_internal::size_type i = 0; i < epi.size(); ++i)
        wsa_events[i] = epi[i].wsa_event;

	int num_ready = 0;
	DWORD wsa_result = 0;
	for(;;) {
		wsa_result = WSAWaitForMultipleEvents(epi.size(), wsa_events, FALSE, 13, FALSE);
		if(wsa_result != WSA_WAIT_TIMEOUT)
			break;
		if(_kbhit()) {
			// console input handle
			epoll_event& ev = events[num_ready++];
			ev.data.ptr = NULL;
			ev.events = FD_READ;
			for(int i = 0; i < epi.size(); i++) {
				if(epi[i].fd == 0) {
					ev.data.ptr = epi[i].event.data.ptr;
					break;
				}
			}
			// if console service not startup, ignore this
			if(ev.data.ptr == NULL) {
				num_ready --;
				continue;
			}
			break;
		}
	}
    if(wsa_result != WSA_WAIT_TIMEOUT) {

        int e = wsa_result - WSA_WAIT_EVENT_0;
        for(ep_internal::size_type i = e; i < epi.size() && num_ready < maxevents; ++i)
        {
            WSANETWORKEVENTS ne;
            if(WSAEnumNetworkEvents(epi[i].fd, wsa_events[i], &ne) != 0) {

				// ignore stdin handle
				if(epi[i].fd == 0)
					continue;
                // error?
                return -1;
			}

			epoll_event& ev = events[num_ready++];
            if(ne.lNetworkEvents != 0) {

                if(epi[i].event.events & EPOLLONESHOT)
                    epi[i].event.events = 0;

				ev.events = (ne.lNetworkEvents & FD_ACCEPT) ? FD_CONNECT : 0;
				ev.events |= (ne.lNetworkEvents & FD_CLOSE) ? (FD_CLOSE | FD_READ) : 0;
				if(ne.lNetworkEvents & FD_READ)
					ev.events |= get_ep_mask(&ne);
				ev.data.ptr = epi[i].event.data.ptr;
            } else {
				// empty event? add an dummy event
				ev.events = 0;
				ev.data.ptr = NULL;
			}
        }
    }
    return num_ready;
}

int epoll_close(int epfd)
{
	GUARD();
    if(epfd < 1 || ep_data.find(epfd) == ep_data.end())
        return ENOENT;

    ep_internal& epi = ep_data[epfd];
    for(ep_internal::size_type i = 0; i < epi.size(); ++i) {

		// now unset the event notifications
		WSAEventSelect(epi[i].fd, 0, 0);
		// clean up event
		WSACloseEvent(epi[i].wsa_event);
	}
    ep_data.erase(epfd);
    return 0;
}

void epoll_cleanup()
{
	GUARD();
    WSACleanup();
	for(std::map<int, ep_internal>::iterator itr = ep_data.begin(); itr != ep_data.end(); ++itr)
		epoll_close(itr->first);
    ep_data.clear();
}

BOOL APIENTRY DllMain( HANDLE hModule, DWORD  ul_reason_for_call,  LPVOID lpReserved) {
  switch (ul_reason_for_call) {
  case DLL_PROCESS_ATTACH: {
	epoll_startup();
  }
  break;

  case DLL_PROCESS_DETACH:
    epoll_cleanup();
    break;
  }
  return TRUE;
}