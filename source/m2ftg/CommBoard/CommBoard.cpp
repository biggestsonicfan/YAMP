#include "CommBoard.h"

#include "../../net/NetPlugin.h"

namespace m2ftg
{
	namespace CommBoard
	{
		void WriteFirmwareStatus(uint8_t* block, bool ringUp, uint8_t nodeId, uint8_t nodeCount,
			const char* tag, const char* waitScreen)
		{
			// One edge detector is enough: one module per process, and one comm block (the
			// local cabinet's board 0) gets the firmware's answer.
			static int s_reported = -1;
			if (s_reported != static_cast<int>(ringUp))
			{
				s_reported = static_cast<int>(ringUp);
				if (ringUp)
				{
					net::Logf("%s: ring UP, node id %u of %u", tag, nodeId, nodeCount);
				}
				else
				{
					net::Logf("%s: ring DOWN, node id %u of %u - the ROM will wait in \"%s\"",
						tag, nodeId, nodeCount, waitScreen);
				}
			}

			for (size_t bank = 0; bank <= BANK; bank += BANK)
			{
				uint8_t* status = block + bank;
				status[STATUS_RING] = ringUp ? 1 : 0;
				status[STATUS_NODE_ID] = nodeId;
				status[STATUS_NODE_COUNT] = nodeCount;
			}
		}
	}
}
