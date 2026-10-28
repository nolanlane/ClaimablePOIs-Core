#pragma once

namespace RE
{
	enum class USER_EVENT_SENDER_ID : std::uint32_t
	{
		None = 0,
		Gameplay = 1,
		Menu = 2,
		Script = 3
	};

	enum class USER_EVENT_FLAG : std::uint32_t
	{
		Walking = (1 << 0),
		Looking = (1 << 1),
		Activation = (1 << 2),    //Unconfirmed
		TabMenuMaybe = (1 << 3),  //Unconfirmed
		Console = (1 << 4),       //Unconfirmed
		POVSwitch = (1 << 5),     //Unconfirmed
		Fighting = (1 << 6),
		Sneaking = (1 << 7),
		Menu = (1 << 8),
		WheelZoom = (1 << 9),  //Unconfirmed
		Jumping = (1 << 10),
		Movement = Walking | Jumping,
	};
	REX_DEFINE_ENUM_CLASS_FLAGS(USER_EVENT_FLAG);

	enum class OTHER_EVENT_FLAG : std::uint32_t
	{
		Journal = (1 << 0),
		Activate = (1 << 1),
		FastTravel = (1 << 2),
		CamSwitch = (1 << 3),
		VATS = (1 << 4),
		Favorites = (1 << 5),
		PipboyLight = (1 << 6),  //Unconfirmed
		ZKey = (1 << 7),
		Running = (1 << 8),
		Unk = (1 << 9),  //??
		Sprinting = (1 << 10),
		HandScanner = (1 << 11),
		Takeoff = (1 << 12),
		Inventory = (1 << 13),
		GravJump = (1 << 14),
		FarTravel = (1 << 15),
		LocationDiscovery = (1 << 16),
		Others = HandScanner | Takeoff | Inventory | GravJump | FarTravel,
	};
	REX_DEFINE_ENUM_CLASS_FLAGS(OTHER_EVENT_FLAG);
}
