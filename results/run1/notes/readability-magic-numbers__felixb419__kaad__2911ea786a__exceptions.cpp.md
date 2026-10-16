The `128` passed to `errmsg.reserve()` was a magic number. I replaced it with a local `constexpr std::size_t initial_errmsg_capacity = 128;`. The reserve size, and so the behaviour, are unchanged.
