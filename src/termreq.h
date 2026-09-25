#pragma once

/**
 * @file termreq.h
 * @brief Requests to a terminal that hosts more than a grid of cells.
 *
 * wbshterm answers OSC 1337: the picker overlay and pane mode are both
 * asked for this way. A terminal that does not know the sequence drops
 * it, so the caller must decide on its own whether asking is worthwhile
 * -- normally by looking for the environment variable wbshterm sets.
 */

#include <string>

namespace wbsh {

	/** Escapes a value for an OSC field, which ends at a ';' or a BEL. */
	std::string percentEncodeRequest(const std::string& text);

	/**
	 * @brief Sends an OSC request to the terminal hosting this console.
	 *
	 * It goes to the console device rather than stdout, because in
	 * `ls | fzf` stdout is the pipe and the terminal would never see it.
	 */
	void writeTerminalRequest(const std::string& request);

	/**
	 * @brief Asks the terminal to forget its scrollback.
	 *
	 * ConPTY does not forward ED 3, so `clear` and Ctrl-L would leave
	 * every old line reachable by scrolling up. Sent only where the
	 * environment says a wbshterm is listening; a no-op elsewhere.
	 */
	void requestScrollbackClear();

}  // namespace wbsh
