//
// publish_subscribe.hpp
// ~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2022 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <vector>
#include <functional>
#include <memory>

#include <boost/signals2.hpp>

const inline int data_length = 2 * 1024 * 1024;

class publish_subscribe
{
public:
	using data_type = std::shared_ptr<std::vector<uint8_t>>;
	using subscribe_func_type = std::function<void(data_type)>;
	using subscribe_list_type = boost::signals2::signal<void(data_type)>;

public:
	publish_subscribe() = default;
	~publish_subscribe() = default;

public:
	boost::signals2::connection subscribe(subscribe_func_type f) noexcept
	{
		return subscribes_.connect(f);
	}

	void unsubscribe(const boost::signals2::connection& slot) noexcept
	{
		slot.disconnect();
	}

	void publish(data_type data) noexcept
	{
		subscribes_(data);
	}

	size_t size() const
	{
		return subscribes_.num_slots();
	}

	void clear() noexcept
	{
		subscribes_.disconnect_all_slots();
	}

private:
	subscribe_list_type subscribes_;
};
