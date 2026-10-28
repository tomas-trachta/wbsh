#pragma once

/**
 * @file stream_relay.h
 * @brief Copying between a socket and a pair of file descriptors, both ways at once (nc).
 *
 * The input side is read on a thread of its own, since Windows cannot
 * wait on a console or a pipe and a socket together; when the relay
 * ends, a read still pending there is cancelled, so nothing is left
 * behind to swallow the shell's next input.
 */

#include "sockets.h"

namespace wbsh {

	struct RelayEnds {
		/// Read until end of file and sent to the socket.
		int input_fd = -1;
		/// fd 0 as the shell sees it: the CRT's read-ahead is sent first, and
		/// a console's CRLF line ends become LF.
		bool input_is_stdin = false;
		/// Set once the input reached its end, so that a later connection
		/// (nc -k) does not wait for it again.
		bool input_exhausted = false;

		/// Receives what the socket delivers; closed when the socket's peer is done.
		int output_fd = -1;
		bool output_is_stdout = false;
	};

	struct RelayPolicy {
		bool udp = false;
		bool end_on_network_eof = true;
		bool shutdown_on_input_eof = false;
		/// Seconds to carry on after the input ends; negative waits for the network.
		double quit_after_input_eof = -1.0;
		/// Seconds without traffic either way before giving up; negative never.
		double idle_timeout = -1.0;
		bool crlf = false;
	};

	enum class RelayEnd { NetworkClosed, InputDone, IdleTimeout, Interrupted, OutputFailed, NetworkFailed };

	RelayEnd relayStreams(NetSocket socket, RelayEnds& ends, const RelayPolicy& policy);

}  // namespace wbsh
