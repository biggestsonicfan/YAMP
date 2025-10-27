#pragma once

namespace Y6 
{
	namespace VF5FS 
	{
		namespace sl
		{

			struct handle_internal_buffer_t
			{
				t_locked_queue_node<handle_internal_buffer_t> m_node;
			};

		}
	}
}