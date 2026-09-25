#include "ModelConfigParser.h"
#include "CVehicleManager.hpp"
#include "HandlingDefault.h"
#include "HandlingManager.h"
#include "extendedveh.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <fstream>
#include <sstream>

namespace HandlingMgr
{

void ModelConfigParser::Trim(std::string& s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
	{
		s.clear();
		return;
	}
	size_t end = s.find_last_not_of(" \t\r\n");
	s = s.substr(start, end - start + 1);
}

std::string ModelConfigParser::ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
		{
			return static_cast<char>(std::tolower(c));
		});
	return s;
}

bool ModelConfigParser::ParseBool(const std::string& val, bool* ok)
{
	std::string lower = ToLower(val);
	Trim(lower);
	if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
	{
		if (ok) *ok = true;
		return true;
	}
	if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
	{
		if (ok) *ok = true;
		return false;
	}
	if (ok) *ok = false;
	return false;
}

uint32_t ModelConfigParser::ParseUInt(const std::string& val, bool* ok)
{
	std::string s = val;
	Trim(s);
	if (s.empty())
	{
		if (ok) *ok = false;
		return 0;
	}

	int base = 10;
	size_t offset = 0;
	if (s.size() > 2 && (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))
	{
		base = 16;
		offset = 2;
	}
	else if (s.size() > 1 && (s[0] == '$' || s[0] == '#'))
	{
		base = 16;
		offset = 1;
	}

	try
	{
		size_t processed = 0;
		unsigned long res = std::stoul(s.substr(offset), &processed, base);
		if (processed != s.size() - offset)
		{
			if (ok) *ok = false;
			return 0;
		}
		if (ok) *ok = true;
		return static_cast<uint32_t>(res);
	}
	catch (...)
	{
		if (ok) *ok = false;
		return 0;
	}
}

int ModelConfigParser::ParseInt(const std::string& val, bool* ok)
{
	std::string s = val;
	Trim(s);
	if (s.empty())
	{
		if (ok) *ok = false;
		return 0;
	}

	int base = 10;
	size_t offset = 0;
	if (s.size() > 2 && (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))
	{
		base = 16;
		offset = 2;
	}
	else if (s.size() > 1 && (s[0] == '$' || s[0] == '#'))
	{
		base = 16;
		offset = 1;
	}

	try
	{
		size_t processed = 0;
		int res = std::stoi(s.substr(offset), &processed, base);
		if (processed != s.size() - offset)
		{
			if (ok) *ok = false;
			return 0;
		}
		if (ok) *ok = true;
		return res;
	}
	catch (...)
	{
		if (ok) *ok = false;
		return 0;
	}
}

float ModelConfigParser::ParseFloat(const std::string& val, bool* ok)
{
	std::string s = val;
	Trim(s);
	if (s.empty())
	{
		if (ok) *ok = false;
		return 0.0f;
	}

	// Strip trailing 'f' or 'F' (e.g. 1200.0f)
	if (s.size() > 1 && (s.back() == 'f' || s.back() == 'F'))
	{
		s.pop_back();
		Trim(s);
	}

	try
	{
		size_t processed = 0;
		float res = std::stof(s, &processed);
		if (processed != s.size() || !std::isfinite(res))
		{
			if (ok) *ok = false;
			return 0.0f;
		}
		if (ok) *ok = true;
		return res;
	}
	catch (...)
	{
		if (ok) *ok = false;
		return 0.0f;
	}
}

std::vector<std::string> ModelConfigParser::Split(const std::string& s, char delim)
{
	std::vector<std::string> tokens;
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, delim))
	{
		Trim(item);
		if (!item.empty())
			tokens.push_back(item);
	}
	return tokens;
}

uint8_t ModelConfigParser::ParseVehicleClass(const std::string& str)
{
	std::string s = ToLower(str);
	Trim(s);
	if (s == "normal")
		return 0;
	if (s == "poorfamily")
		return 1;
	if (s == "richfamily")
		return 2;
	if (s == "executive")
		return 3;
	if (s == "worker")
		return 4;
	if (s == "special")
		return 5;
	if (s == "big")
		return 6;
	if (s == "taxi")
		return 7;
	if (s == "moped")
		return 8;
	if (s == "motorbike")
		return 9;
	if (s == "leisureboat")
		return 10;
	if (s == "workerboat")
		return 11;
	if (s == "bicycle")
		return 12;
	if (s == "ignore")
		return 13;

	try
	{
		int v = std::stoi(s);
		if (v >= 0 && v <= 13)
			return static_cast<uint8_t>(v);
	}
	catch (...)
	{
	}

	return 0;
}

int ModelConfigParser::ResolveVehicleModelId(const std::string& nameOrId)
{
	std::string s = nameOrId;
	Trim(s);
	if (s.empty())
		return -1;

	try
	{
		int id = std::stoi(s);
		if ((id >= 400 && id <= 611) || (id >= CVehicleMgr::CUSTOM_MODEL_START && id <= CVehicleMgr::MAX_NETWORK_VEHICLES))
			return id;
	}
	catch (...)
	{
	}

	std::string lower = ToLower(s);

	// Check custom vehicle names registered in customVehicleConfigs
	for (const auto& [customId, cfg] : customVehicleConfigs)
	{
		if (!cfg.name.empty() && ToLower(cfg.name) == lower)
		{
			return static_cast<int>(customId);
		}
	}

	static const std::unordered_map<std::string, int> s_vehicleNames = {
		{ "landstalker", 400 }, { "bravura", 401 }, { "buffalo", 402 }, { "linerunner", 403 },
		{ "perennial", 404 }, { "sentinel", 405 }, { "dumper", 406 }, { "firetruck", 407 },
		{ "trashmaster", 408 }, { "stretch", 409 }, { "manana", 410 }, { "infernus", 411 },
		{ "voodoo", 412 }, { "pony", 413 }, { "mule", 414 }, { "cheetah", 415 },
		{ "ambulance", 416 }, { "leviathan", 417 }, { "moonbeam", 418 }, { "esperanto", 419 },
		{ "taxi", 420 }, { "washington", 421 }, { "bobcat", 422 }, { "mrwhoopee", 423 },
		{ "bf_injection", 424 }, { "hunter", 425 }, { "premier", 426 }, { "enforcer", 427 },
		{ "securicar", 428 }, { "banshee", 429 }, { "predator", 430 }, { "bus", 431 },
		{ "rhino", 432 }, { "barracks", 433 }, { "hotknife", 434 }, { "trailer1", 435 },
		{ "previon", 436 }, { "coach", 437 }, { "cabbie", 438 }, { "stallion", 439 },
		{ "rumpo", 440 }, { "rcbandit", 441 }, { "romero", 442 }, { "packer", 443 },
		{ "monster", 444 }, { "admiral", 445 }, { "squalo", 446 }, { "seasparrow", 447 },
		{ "pizzaboy", 448 }, { "tram", 449 }, { "trailer2", 450 }, { "turismo", 451 },
		{ "speeder", 452 }, { "reefer", 453 }, { "tropic", 454 }, { "flatbed", 455 },
		{ "yankee", 456 }, { "caddy", 457 }, { "solair", 458 }, { "topfun", 459 },
		{ "skimmer", 460 }, { "pcj-600", 461 }, { "pcj600", 461 }, { "faggio", 462 },
		{ "freeway", 463 }, { "rcbaron", 464 }, { "rcraider", 465 }, { "glendale", 466 },
		{ "oceanic", 467 }, { "sanchez", 468 }, { "sparrow", 469 }, { "patriot", 470 },
		{ "quad", 471 }, { "coastguard", 472 }, { "dinghy", 473 }, { "hermes", 474 },
		{ "sabre", 475 }, { "rustler", 476 }, { "zr-350", 477 }, { "zr350", 477 },
		{ "walton", 478 }, { "regina", 479 }, { "comet", 480 }, { "bmx", 481 },
		{ "burrito", 482 }, { "camper", 483 }, { "marquis", 484 }, { "baggage", 485 },
		{ "dozer", 486 }, { "maverick", 487 }, { "news_chopper", 488 }, { "rancher", 489 },
		{ "fbi_rancher", 490 }, { "virgo", 491 }, { "greenwood", 492 }, { "jetmax", 493 },
		{ "hotring_racer", 494 }, { "sandking", 495 }, { "blista_compact", 496 },
		{ "police_maverick", 497 }, { "boxville", 498 }, { "benson", 499 }, { "mesa", 500 },
		{ "rcgoblin", 501 }, { "hotring_racer2", 502 }, { "hotring_racer3", 503 },
		{ "bloodring_banger", 504 }, { "rancher2", 505 }, { "super_gt", 506 },
		{ "elegant", 507 }, { "journey", 508 }, { "bike", 509 }, { "mountain_bike", 510 },
		{ "beagle", 511 }, { "cropduster", 512 }, { "stuntplane", 513 }, { "petro_trailer", 514 },
		{ "roadtrain", 515 }, { "nebula", 516 }, { "majestic", 517 }, { "buccaneer", 518 },
		{ "shamal", 519 }, { "hydra", 520 }, { "fcr-900", 521 }, { "fcr900", 521 },
		{ "nrg-500", 522 }, { "nrg500", 522 }, { "copbike", 523 }, { "cement_truck", 524 },
		{ "towtruck", 525 }, { "fortune", 526 }, { "cadrona", 527 }, { "fbi_truck", 528 },
		{ "willard", 529 }, { "forklift", 530 }, { "tractor", 531 }, { "combine_harvester", 532 },
		{ "feltzer", 533 }, { "remington", 534 }, { "slamvan", 535 }, { "blade", 536 },
		{ "freight", 537 }, { "streak", 538 }, { "vortex", 539 }, { "vincent", 540 },
		{ "bullet", 541 }, { "clover", 542 }, { "sadler", 543 }, { "firetruck_ladder", 544 },
		{ "hustler", 545 }, { "intruder", 546 }, { "primo", 547 }, { "cargobob", 548 },
		{ "tampa", 549 }, { "sunrise", 550 }, { "merit", 551 }, { "utility_van", 552 },
		{ "nevada", 553 }, { "yosemite", 554 }, { "windsor", 555 }, { "monster2", 556 },
		{ "monster3", 557 }, { "uranus", 558 }, { "jester", 559 }, { "sultan", 560 },
		{ "stratum", 561 }, { "elegy", 562 }, { "raindance", 563 }, { "rc_tiger", 564 },
		{ "flash", 565 }, { "tahoma", 566 }, { "savanna", 567 }, { "bandito", 568 },
		{ "freight_flat", 569 }, { "streak_carriage", 570 }, { "kart", 571 }, { "mower", 572 },
		{ "dune", 573 }, { "sweeper", 574 }, { "broadway", 575 }, { "tornado", 576 },
		{ "at-400", 577 }, { "at400", 577 }, { "dft-30", 578 }, { "huntley", 579 },
		{ "stafford", 580 }, { "bf-400", 581 }, { "newsvan", 582 }, { "tug", 583 },
		{ "petrol_tanker", 584 }, { "emperor", 585 }, { "wayfarer", 586 }, { "euros", 587 },
		{ "hotdog", 588 }, { "club", 589 }, { "box_freight", 590 }, { "trailer3", 591 },
		{ "andromada", 592 }, { "dodo", 593 }, { "rc_cam", 594 }, { "launch", 595 },
		{ "police_ls", 596 }, { "police_sf", 597 }, { "police_lv", 598 }, { "police_ranger", 599 },
		{ "picador", 600 }, { "swat_tank", 601 }, { "alpha", 602 }, { "phoenix", 603 },
		{ "glendale_shit", 604 }, { "sadler_shit", 605 }, { "luggage_trailer1", 606 },
		{ "luggage_trailer2", 607 }, { "stairs_trailer", 608 }, { "boxville_mission", 609 },
		{ "farm_trailer", 610 }, { "utility_trailer", 611 }
	};

	auto it = s_vehicleNames.find(lower);
	if (it != s_vehicleNames.end())
		return it->second;

	return -1;
}

int ModelConfigParser::ResolveUpgradeComponentId(const std::string& partName)
{
	std::string s = ToLower(partName);
	Trim(s);
	if (s.empty())
		return -1;

	try
	{
		int id = std::stoi(s);
		if (id >= 1000 && id <= 1193)
			return id;
	}
	catch (...)
	{
	}

	static const std::unordered_map<std::string, int> s_modMap = {
		{ "nto_b_tw", 1008 }, { "nto_b_s", 1009 }, { "nto_b_l", 1010 },
		{ "hydralics", 1087 }, { "hydraulics", 1087 }, { "stereo", 1086 },
		{ "wheel_shadow", 1073 }, { "wheel_mega", 1074 }, { "wheel_rimshine", 1075 },
		{ "wheel_wires", 1076 }, { "wheel_classic", 1077 }, { "wheel_twist", 1078 },
		{ "wheel_cutter", 1079 }, { "wheel_switch", 1080 }, { "wheel_grove", 1081 },
		{ "wheel_import", 1082 }, { "wheel_dollar", 1083 }, { "wheel_trance", 1084 },
		{ "wheel_atomic", 1085 }, { "wheel_access", 1096 }, { "wheel_virtual", 1097 },
		{ "wheel_ahab", 1098 }
	};

	auto it = s_modMap.find(s);
	if (it != s_modMap.end())
		return it->second;

	return -1;
}

bool ModelConfigParser::ParseFile(const fs::path& filePath, ModelConfig& outConfig, uint32_t fallbackBaseModel)
{
	std::ifstream file(filePath);
	if (!file.is_open())
		return false;

	std::stringstream buffer;
	buffer << file.rdbuf();
	return ParseString(buffer.str(), outConfig, fallbackBaseModel, filePath.string());
}

bool ModelConfigParser::ParseString(const std::string& content, ModelConfig& outConfig, uint32_t fallbackBaseModel, const std::string& sourceName)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;

	uint16_t baseModel = static_cast<uint16_t>(fallbackBaseModel);
	HandlingDefault::copyDefaultModelHandling(baseModel, &outConfig.handlingData);

	std::stringstream ss(content);
	std::string rawLine;
	std::string currentSection = "model";
	int lineNum = 0;

	auto logWarn = [&](const char* fmt, ...)
	{
		if (!core_)
			return;
		char msgBuf[512];
		va_list args;
		va_start(args, fmt);
		vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
		va_end(args);
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] %s (line %d): %s", sourceName.c_str(), lineNum, msgBuf);
	};

	while (std::getline(ss, rawLine))
	{
		++lineNum;
		std::string line = rawLine;
		Trim(line);
		if (line.empty())
			continue;

		// Whole-line comments: #, ;, //
		if (line[0] == '#' || line[0] == ';' || (line.size() >= 2 && line[0] == '/' && line[1] == '/'))
			continue;

		// Inline comments: support ' #', ' ;', or ' //' preceded by whitespace so values with ';' (like colors) are safe
		size_t inlineComment = std::string::npos;
		for (size_t i = 1; i < line.size(); ++i)
		{
			if (std::isspace(static_cast<unsigned char>(line[i - 1])))
			{
				if (line[i] == '#' || line[i] == ';')
				{
					inlineComment = i - 1;
					break;
				}
				if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/')
				{
					inlineComment = i - 1;
					break;
				}
			}
		}
		if (inlineComment != std::string::npos)
		{
			line = line.substr(0, inlineComment);
			Trim(line);
			if (line.empty())
				continue;
		}

		// Section header
		if (line.front() == '[' && line.back() == ']')
		{
			currentSection = ToLower(line.substr(1, line.size() - 2));
			Trim(currentSection);
			continue;
		}

		// Key-value pair
		size_t eqPos = line.find('=');
		if (eqPos == std::string::npos)
			eqPos = line.find(':');
		if (eqPos == std::string::npos)
			continue;

		std::string key = line.substr(0, eqPos);
		std::string val = line.substr(eqPos + 1);
		Trim(key);
		Trim(val);
		if (key.empty())
			continue;

		std::string lowerKey = ToLower(key);

		// Store in custom properties registry
		outConfig.customProperties[lowerKey] = val;
		outConfig.customProperties[currentSection + "." + lowerKey] = val;

		try
		{
			// Section dispatch
			if (currentSection == "model" || currentSection == "general" || currentSection == "base")
			{
				if (lowerKey == "name" || lowerKey == "friendlyname")
				{
					outConfig.name = val;
				}
				else if (lowerKey == "visualbase" || lowerKey == "visualbasemodel")
				{
					int id = ResolveVehicleModelId(val);
					if (id >= 400 && id <= 611)
						outConfig.visualBase = static_cast<uint32_t>(id);
					else
						logWarn("Invalid visualBase '%s' (expected 400..611 or valid SA vehicle name)", val.c_str());
				}
				else if (lowerKey == "audiobase" || lowerKey == "audiobasemodel")
				{
					int id = ResolveVehicleModelId(val);
					if (id > 0)
						outConfig.audioBase = static_cast<uint32_t>(id);
					else
						logWarn("Invalid audioBase '%s' (expected 400..611, custom model ID, or valid vehicle name)", val.c_str());
				}
				else if (lowerKey == "handlingbase" || lowerKey == "handlingbasemodel")
				{
					int id = ResolveVehicleModelId(val);
					if (id > 0)
					{
						outConfig.handlingBase = static_cast<uint32_t>(id);
						HandlingDefault::copyDefaultModelHandling(static_cast<uint16_t>(id), &outConfig.handlingData);
					}
					else
					{
						logWarn("Invalid handlingBase '%s' (expected 400..611, custom model ID, or valid vehicle name)", val.c_str());
					}
				}
				else if (lowerKey == "engineonsound" || lowerKey == "soundon")
				{
					bool ok = false;
					int snd = ParseInt(val, &ok);
					if (ok && snd >= -1 && snd <= 32767)
						outConfig.engineOnSound = static_cast<int16_t>(snd);
					else
						logWarn("Invalid engineOnSound '%s' (expected -1..32767)", val.c_str());
				}
				else if (lowerKey == "engineoffsound" || lowerKey == "soundoff")
				{
					bool ok = false;
					int snd = ParseInt(val, &ok);
					if (ok && snd >= -1 && snd <= 32767)
						outConfig.engineOffSound = static_cast<int16_t>(snd);
					else
						logWarn("Invalid engineOffSound '%s' (expected -1..32767)", val.c_str());
				}
				else if (lowerKey == "acceleratesound")
				{
					bool ok = false;
					int snd = ParseInt(val, &ok);
					if (ok && snd >= -1 && snd <= 32767)
						outConfig.accelerateSound = static_cast<int16_t>(snd);
					else
						logWarn("Invalid accelerateSound '%s' (expected -1..32767)", val.c_str());
				}
				else if (lowerKey == "deceleratesound")
				{
					bool ok = false;
					int snd = ParseInt(val, &ok);
					if (ok && snd >= -1 && snd <= 32767)
						outConfig.decelerateSound = static_cast<int16_t>(snd);
					else
						logWarn("Invalid decelerateSound '%s' (expected -1..32767)", val.c_str());
				}
			}
			else if (currentSection == "ide" || currentSection == "vehicle" || currentSection == "vehicles")
			{
				outConfig.hasIde = true;
				if (lowerKey == "vehicletype" || lowerKey == "type")
				{
					outConfig.vehicleType = val;
				}
				else if (lowerKey == "vehicleclass" || lowerKey == "class")
				{
					outConfig.vehicleClass = ParseVehicleClass(val);
				}
				else if (lowerKey == "frequency" || lowerKey == "freq")
				{
					bool ok = false;
					uint32_t f = ParseUInt(val, &ok);
					if (ok && f <= 100)
						outConfig.frequency = static_cast<uint16_t>(f);
					else
						logWarn("Invalid frequency '%s' (expected 0..100)", val.c_str());
				}
				else if (lowerKey == "level")
				{
					bool ok = false;
					uint32_t l = ParseUInt(val, &ok);
					if (ok && l <= 10)
						outConfig.level = static_cast<uint8_t>(l);
					else
						logWarn("Invalid level '%s' (expected 0..10)", val.c_str());
				}
				else if (lowerKey == "comprules" || lowerKey == "comprate")
				{
					bool ok = false;
					uint32_t cr = ParseUInt(val, &ok);
					if (ok && cr <= 255)
						outConfig.compRules = static_cast<uint8_t>(cr);
					else
						logWarn("Invalid compRules '%s' (expected 0..255)", val.c_str());
				}
				else if (lowerKey == "wheelmodelid" || lowerKey == "wheelmodel" || lowerKey == "wheelid")
				{
					bool ok = false;
					int wid = ParseInt(val, &ok);
					if (ok && (wid == -1 || wid == 1025 || (wid >= 1073 && wid <= 1098)))
						outConfig.wheelModelId = static_cast<int16_t>(wid);
					else
						logWarn("Invalid wheelModelId '%s' (expected -1, 1025, or 1073..1098)", val.c_str());
				}
				else if (lowerKey == "wheelscalefront" || lowerKey == "frontwheelscale")
				{
					bool ok = false;
					float ws = ParseFloat(val, &ok);
					if (ok && ws >= 0.05f && ws <= 5.0f)
						outConfig.wheelScaleFront = ws;
					else
						logWarn("Invalid wheelScaleFront '%s' (expected range 0.05..5.0)", val.c_str());
				}
				else if (lowerKey == "wheelscalerear" || lowerKey == "rearwheelscale")
				{
					bool ok = false;
					float ws = ParseFloat(val, &ok);
					if (ok && ws >= 0.05f && ws <= 5.0f)
						outConfig.wheelScaleRear = ws;
					else
						logWarn("Invalid wheelScaleRear '%s' (expected range 0.05..5.0)", val.c_str());
				}
				else if (lowerKey == "wheelscale")
				{
					bool ok = false;
					float ws = ParseFloat(val, &ok);
					if (ok && ws >= 0.05f && ws <= 5.0f)
					{
						outConfig.wheelScaleFront = ws;
						outConfig.wheelScaleRear = ws;
					}
					else
						logWarn("Invalid wheelScale '%s' (expected range 0.05..5.0)", val.c_str());
				}
				else if (lowerKey == "wheelupgradeclass")
				{
					bool ok = false;
					uint32_t wuc = ParseUInt(val, &ok);
					if (ok && wuc <= 255)
						outConfig.wheelUpgradeClass = static_cast<uint8_t>(wuc);
					else
						logWarn("Invalid wheelUpgradeClass '%s' (expected 0..255)", val.c_str());
				}
				else if (lowerKey == "numextras")
				{
					bool ok = false;
					uint32_t ne = ParseUInt(val, &ok);
					if (ok && ne <= 6)
						outConfig.numExtras = static_cast<uint8_t>(ne);
					else
						logWarn("Invalid numExtras '%s' (expected 0..6)", val.c_str());
				}
			}
			else if (currentSection == "handling")
			{
				outConfig.hasHandling = true;
				auto recordMod = [&](CHandlingAttrib attrib, CHandlingAttribType type, auto value)
				{
					stHandlingMod mod {};
					mod.type = type;
					if constexpr (std::is_floating_point_v<decltype(value)>)
						mod.fval = static_cast<float>(value);
					else if constexpr (std::is_same_v<decltype(value), uint8_t> || std::is_same_v<decltype(value), char>)
						mod.bval = static_cast<uint8_t>(value);
					else
						mod.uival = static_cast<unsigned int>(value);
					outConfig.handlingMods[attrib] = mod;
				};

				if (lowerKey == "mass")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FMASS, f))
					{
						outConfig.handlingData.m_fMass = f;
						recordMod(HANDL_FMASS, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid mass '%s' (valid range 1.0..50000.0 kg). Retaining base vehicle mass (%.1f kg)", val.c_str(), outConfig.handlingData.m_fMass);
					}
				}
				else if (lowerKey == "turnmass")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FTURNMASS, f))
					{
						outConfig.handlingData.m_fTurnMass = f;
						recordMod(HANDL_FTURNMASS, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid turnMass '%s' (valid range 0.0..50000.0 kg). Retaining base vehicle turnMass (%.1f kg)", val.c_str(), outConfig.handlingData.m_fTurnMass);
					}
				}
				else if (lowerKey == "dragmult" || lowerKey == "dragmultiplier")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FDRAGMULTIPLIER, f))
					{
						outConfig.handlingData.m_fDragMult = f;
						recordMod(HANDL_FDRAGMULTIPLIER, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid dragMultiplier '%s' (valid range -200.0..200.0)", val.c_str());
					}
				}
				else if (lowerKey == "centreofmassx" || lowerKey == "centerofmassx" || lowerKey == "comx")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_CENTREOFMASS_X, f))
					{
						outConfig.handlingData.m_vecCentreOfMass.fX = f;
						recordMod(HANDL_CENTREOFMASS_X, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid centreOfMassX '%s' (valid range -10.0..10.0)", val.c_str());
					}
				}
				else if (lowerKey == "centreofmassy" || lowerKey == "centerofmassy" || lowerKey == "comy")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_CENTREOFMASS_Y, f))
					{
						outConfig.handlingData.m_vecCentreOfMass.fY = f;
						recordMod(HANDL_CENTREOFMASS_Y, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid centreOfMassY '%s' (valid range -10.0..10.0)", val.c_str());
					}
				}
				else if (lowerKey == "centreofmassz" || lowerKey == "centerofmassz" || lowerKey == "comz")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_CENTREOFMASS_Z, f))
					{
						outConfig.handlingData.m_vecCentreOfMass.fZ = f;
						recordMod(HANDL_CENTREOFMASS_Z, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid centreOfMassZ '%s' (valid range -10.0..10.0)", val.c_str());
					}
				}
				else if (lowerKey == "percentsubmerged" || lowerKey == "submerged")
				{
					bool ok = false;
					uint32_t u = ParseUInt(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_NPERCENTSUBMERGED, static_cast<uint8_t>(u)))
					{
						outConfig.handlingData.m_nPercentSubmerged = static_cast<uint8_t>(u);
						recordMod(HANDL_NPERCENTSUBMERGED, TYPE_BYTE, outConfig.handlingData.m_nPercentSubmerged);
					}
					else
					{
						logWarn("Invalid percentSubmerged '%s' (valid range 1..255)", val.c_str());
					}
				}
				else if (lowerKey == "tractionmultiplier" || lowerKey == "tractionmult")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FTRACTIONMULTIPLIER, f))
					{
						outConfig.handlingData.m_fTractionMultiplier = f;
						recordMod(HANDL_FTRACTIONMULTIPLIER, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid tractionMultiplier '%s' (valid range -100.0..100.0)", val.c_str());
					}
				}
				else if (lowerKey == "tractionloss")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FTRACTIONLOSS, f))
					{
						outConfig.handlingData.m_fTractionLoss = f;
						recordMod(HANDL_FTRACTIONLOSS, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid tractionLoss '%s' (valid range 0.0..100.0)", val.c_str());
					}
				}
				else if (lowerKey == "tractionbias")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FTRACTIONBIAS, f))
					{
						outConfig.handlingData.m_fTractionBias = f;
						recordMod(HANDL_FTRACTIONBIAS, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid tractionBias '%s' (valid range 0.0..1.0)", val.c_str());
					}
				}
				else if (lowerKey == "numberofgears" || lowerKey == "gears")
				{
					bool ok = false;
					uint32_t u = ParseUInt(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_TR_NNUMBEROFGEARS, static_cast<uint8_t>(u)))
					{
						outConfig.handlingData.m_transmissionData.m_nNumberOfGears = static_cast<unsigned char>(u);
						recordMod(HANDL_TR_NNUMBEROFGEARS, TYPE_BYTE, outConfig.handlingData.m_transmissionData.m_nNumberOfGears);
					}
					else
					{
						logWarn("Invalid numberOfGears '%s' (valid range 1..6). Retaining base vehicle gears (%u)", val.c_str(), outConfig.handlingData.m_transmissionData.m_nNumberOfGears);
					}
				}
				else if (lowerKey == "maxvelocity" || lowerKey == "topspeed")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_TR_FMAXVELOCITY, f))
					{
						outConfig.handlingData.m_transmissionData.m_fMaxGearVelocity = f;
						recordMod(HANDL_TR_FMAXVELOCITY, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid maxVelocity '%s' (valid range 0.1..10000.0)", val.c_str());
					}
				}
				else if (lowerKey == "engineacceleration" || lowerKey == "acceleration")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_TR_FENGINEACCELERATION, f))
					{
						outConfig.handlingData.m_transmissionData.m_fEngineAcceleration = f;
						recordMod(HANDL_TR_FENGINEACCELERATION, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid engineAcceleration '%s' (valid range 0.0..10000.0)", val.c_str());
					}
				}
				else if (lowerKey == "engineinertia" || lowerKey == "inertia")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_TR_FENGINEINERTIA, f))
					{
						outConfig.handlingData.m_transmissionData.m_fEngineInertia = f;
						recordMod(HANDL_TR_FENGINEINERTIA, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid engineInertia '%s' (valid range -1000.0..1000.0, non-zero)", val.c_str());
					}
				}
				else if (lowerKey == "drivetype")
				{
					char dt = val.empty() ? '?' : static_cast<char>(std::toupper(static_cast<unsigned char>(val[0])));
					if (IsValidHandlingValue(HANDL_TR_NDRIVETYPE, static_cast<uint8_t>(dt)))
					{
						outConfig.handlingData.m_transmissionData.m_nDriveType = static_cast<unsigned char>(dt);
						recordMod(HANDL_TR_NDRIVETYPE, TYPE_BYTE, outConfig.handlingData.m_transmissionData.m_nDriveType);
					}
					else
					{
						logWarn("Invalid driveType '%s' (expected 'F', 'R', or '4'). Retaining base vehicle driveType ('%c')", val.c_str(), outConfig.handlingData.m_transmissionData.m_nDriveType);
					}
				}
				else if (lowerKey == "enginetype")
				{
					char et = val.empty() ? '?' : static_cast<char>(std::toupper(static_cast<unsigned char>(val[0])));
					if (IsValidHandlingValue(HANDL_TR_NENGINETYPE, static_cast<uint8_t>(et)))
					{
						outConfig.handlingData.m_transmissionData.m_nEngineType = static_cast<unsigned char>(et);
						recordMod(HANDL_TR_NENGINETYPE, TYPE_BYTE, outConfig.handlingData.m_transmissionData.m_nEngineType);
					}
					else
					{
						logWarn("Invalid engineType '%s' (expected 'P' [petrol], 'D' [diesel], or 'E' [electric]). Retaining base vehicle engineType ('%c')", val.c_str(), outConfig.handlingData.m_transmissionData.m_nEngineType);
					}
				}
				else if (lowerKey == "brakedeceleration")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FBRAKEDECELERATION, f))
					{
						outConfig.handlingData.m_fBrakeDeceleration = f;
						recordMod(HANDL_FBRAKEDECELERATION, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid brakeDeceleration '%s' (valid range 0.1..10000.0)", val.c_str());
					}
				}
				else if (lowerKey == "brakebias")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FBRAKEBIAS, f))
					{
						outConfig.handlingData.m_fBrakeBias = f;
						recordMod(HANDL_FBRAKEBIAS, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid brakeBias '%s' (valid range 0.0..1.0)", val.c_str());
					}
				}
				else if (lowerKey == "abs")
				{
					bool ok = false;
					bool b = ParseBool(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_BABS, static_cast<uint8_t>(b ? 1 : 0)))
					{
						outConfig.handlingData.m_bABS = b ? 1 : 0;
						recordMod(HANDL_BABS, TYPE_BYTE, static_cast<uint8_t>(outConfig.handlingData.m_bABS));
					}
					else
					{
						logWarn("Invalid abs '%s' (expected true/false or 1/0)", val.c_str());
					}
				}
				else if (lowerKey == "steeringlock")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FSTEERINGLOCK, f))
					{
						outConfig.handlingData.m_fSteeringLock = f;
						recordMod(HANDL_FSTEERINGLOCK, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid steeringLock '%s' (valid range 0.0..360.0 degrees)", val.c_str());
					}
				}
				else if (lowerKey == "suspensionforcelevel")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FSUSPENSIONFORCELEVEL, f))
					{
						outConfig.handlingData.m_fSuspensionForceLevel = f;
						recordMod(HANDL_FSUSPENSIONFORCELEVEL, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid suspensionForceLevel '%s' (valid range 0.0..600.0)", val.c_str());
					}
				}
				else if (lowerKey == "suspensiondampinglevel")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FSUSPENSIONDAMPINGLEVEL, f))
					{
						outConfig.handlingData.m_fSuspensionDampingLevel = f;
						recordMod(HANDL_FSUSPENSIONDAMPINGLEVEL, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid suspensionDampingLevel '%s' (valid range 0.0..600.0)", val.c_str());
					}
				}
				else if (lowerKey == "suspensionhighspdcomdamp")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FSUSPENSIONHIGHSPDCOMDAMP, f))
					{
						outConfig.handlingData.m_fSuspensionHighSpdComDamp = f;
						recordMod(HANDL_FSUSPENSIONHIGHSPDCOMDAMP, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid suspensionHighSpdComDamp '%s' (valid range 0.0..600.0)", val.c_str());
					}
				}
				else if (lowerKey == "suspensionupperlimit")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FSUSPENSIONUPPERLIMIT, f))
					{
						outConfig.handlingData.m_fSuspensionUpperLimit = f;
						recordMod(HANDL_FSUSPENSIONUPPERLIMIT, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid suspensionUpperLimit '%s' (valid range -50.0..50.0)", val.c_str());
					}
				}
				else if (lowerKey == "suspensionlowerlimit")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FSUSPENSIONLOWERLIMIT, f))
					{
						outConfig.handlingData.m_fSuspensionLowerLimit = f;
						recordMod(HANDL_FSUSPENSIONLOWERLIMIT, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid suspensionLowerLimit '%s' (valid range -50.0..50.0)", val.c_str());
					}
				}
				else if (lowerKey == "suspensionbias")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FSUSPENSIONBIAS, f))
					{
						outConfig.handlingData.m_fSuspensionBiasBetweenFrontAndRear = f;
						recordMod(HANDL_FSUSPENSIONBIAS, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid suspensionBias '%s' (valid range 0.0..1.0)", val.c_str());
					}
				}
				else if (lowerKey == "suspensionantidivemultiplier")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FSUSPENSIONANTIDIVEMULT, f))
					{
						outConfig.handlingData.m_fSuspensionAntiDiveMultiplier = f;
						recordMod(HANDL_FSUSPENSIONANTIDIVEMULT, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid suspensionAntiDiveMultiplier '%s' (valid range 0.0..30.0)", val.c_str());
					}
				}
				else if (lowerKey == "seatoffsetdistance")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FSEATOFFSETDISTANCE, f))
					{
						outConfig.handlingData.m_fSeatOffsetDistance = f;
						recordMod(HANDL_FSEATOFFSETDISTANCE, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid seatOffsetDistance '%s' (valid range -20.0..20.0)", val.c_str());
					}
				}
				else if (lowerKey == "collisiondamagemultiplier")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FCOLLISIONDAMAGEMULT, f))
					{
						outConfig.handlingData.m_fCollisionDamageMultiplier = f;
						recordMod(HANDL_FCOLLISIONDAMAGEMULT, TYPE_FLOAT, f);
					}
					else
					{
						logWarn("Invalid collisionDamageMultiplier '%s' (valid range 0.0..10.0)", val.c_str());
					}
				}
				else if (lowerKey == "monetaryvalue")
				{
					bool ok = false;
					uint32_t u = ParseUInt(val, &ok);
					if (ok)
					{
						outConfig.handlingData.m_nMonetaryValue = u;
						recordMod(HANDL_UIMONETARYVALUE, TYPE_UINT, u);
					}
					else
					{
						logWarn("Invalid monetaryValue '%s'", val.c_str());
					}
				}
				else if (lowerKey == "modelflags")
				{
					bool ok = false;
					uint32_t u = ParseUInt(val, &ok);
					if (ok)
					{
						outConfig.handlingData.m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(u);
						recordMod(HANDL_MODELFLAGS, TYPE_FLAG, u);
					}
					else
					{
						logWarn("Invalid modelFlags '%s'", val.c_str());
					}
				}
				else if (lowerKey == "handlingflags")
				{
					bool ok = false;
					uint32_t u = ParseUInt(val, &ok);
					if (ok)
					{
						outConfig.handlingData.m_nHandlingFlags = static_cast<eVehicleHandlingFlags>(u);
						recordMod(HANDL_HANDLINGFLAGS, TYPE_FLAG, u);
					}
					else
					{
						logWarn("Invalid handlingFlags '%s'", val.c_str());
					}
				}
				else if (lowerKey == "frontlights" || lowerKey == "frontlight")
				{
					bool ok = false;
					uint32_t u = ParseUInt(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_FRONTLIGHTS, static_cast<uint8_t>(u)))
					{
						outConfig.handlingData.m_nFrontLights = static_cast<eVehicleLightsSize>(u);
						recordMod(HANDL_FRONTLIGHTS, TYPE_BYTE, static_cast<uint8_t>(outConfig.handlingData.m_nFrontLights));
					}
					else
					{
						logWarn("Invalid frontLights '%s' (expected 0..3: 0=long, 1=small, 2=big, 3=tall)", val.c_str());
					}
				}
				else if (lowerKey == "rearlights" || lowerKey == "rearlight")
				{
					bool ok = false;
					uint32_t u = ParseUInt(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_REARLIGHTS, static_cast<uint8_t>(u)))
					{
						outConfig.handlingData.m_nRearLights = static_cast<eVehicleLightsSize>(u);
						recordMod(HANDL_REARLIGHTS, TYPE_BYTE, static_cast<uint8_t>(outConfig.handlingData.m_nRearLights));
					}
					else
					{
						logWarn("Invalid rearLights '%s' (expected 0..3: 0=long, 1=small, 2=big, 3=tall)", val.c_str());
					}
				}
				else if (lowerKey == "animgroup")
				{
					bool ok = false;
					uint32_t u = ParseUInt(val, &ok);
					if (ok && IsValidHandlingValue(HANDL_ANIMGROUP, static_cast<uint8_t>(u)))
					{
						outConfig.handlingData.m_nAnimGroup = static_cast<unsigned char>(u);
						recordMod(HANDL_ANIMGROUP, TYPE_BYTE, outConfig.handlingData.m_nAnimGroup);
					}
					else
					{
						logWarn("Invalid animGroup '%s' (valid range 0..255)", val.c_str());
					}
				}
			}
			else if (currentSection == "carmods" || currentSection == "mods")
			{
				outConfig.hasCarmods = true;
				std::vector<std::string> parts = Split(val, ',');
				if (parts.empty())
					parts = Split(val, ' ');

				std::vector<std::string> validParts;
				for (const auto& part : parts)
				{
					int compId = ResolveUpgradeComponentId(part);
					if (compId >= 1000 && compId <= 1193)
					{
						validParts.push_back(part);
						if (std::find(outConfig.modNames.begin(), outConfig.modNames.end(), part) == outConfig.modNames.end())
							outConfig.modNames.push_back(part);
						if (std::find(outConfig.modIds.begin(), outConfig.modIds.end(), compId) == outConfig.modIds.end())
							outConfig.modIds.push_back(compId);
					}
					else
					{
						logWarn("Unrecognized or invalid carmod part '%s' (expected 1000..1193 or valid part name)", part.c_str());
					}
				}
				if (!validParts.empty())
					outConfig.categorizedMods[lowerKey] = validParts;
			}
			else if (currentSection == "carcols" || currentSection == "colors")
			{
				outConfig.hasCarcols = true;
				if (lowerKey == "primary")
				{
					bool ok = false;
					uint32_t c = ParseUInt(val, &ok);
					if (ok && c <= 255)
						outConfig.defaultPrimaryColor = static_cast<uint8_t>(c);
					else
						logWarn("Invalid primary color '%s' (expected 0..255)", val.c_str());
				}
				else if (lowerKey == "secondary")
				{
					bool ok = false;
					uint32_t c = ParseUInt(val, &ok);
					if (ok && c <= 255)
						outConfig.defaultSecondaryColor = static_cast<uint8_t>(c);
					else
						logWarn("Invalid secondary color '%s' (expected 0..255)", val.c_str());
				}
				else if (lowerKey == "tertiary")
				{
					bool ok = false;
					uint32_t c = ParseUInt(val, &ok);
					if (ok && c <= 255)
						outConfig.defaultTertiaryColor = static_cast<uint8_t>(c);
					else
						logWarn("Invalid tertiary color '%s' (expected 0..255)", val.c_str());
				}
				else if (lowerKey == "quaternary")
				{
					bool ok = false;
					uint32_t c = ParseUInt(val, &ok);
					if (ok && c <= 255)
						outConfig.defaultQuaternaryColor = static_cast<uint8_t>(c);
					else
						logWarn("Invalid quaternary color '%s' (expected 0..255)", val.c_str());
				}
				else if (lowerKey == "variations")
				{
					std::vector<std::string> varList = Split(val, ';');
					for (const auto& rawVStr : varList)
					{
						std::string vStr = rawVStr;
						Trim(vStr);
						if (vStr.empty())
							continue;

						std::vector<std::string> c = Split(vStr, ',');
						if (c.size() >= 2)
						{
							bool ok0 = false, ok1 = false;
							uint32_t c0 = ParseUInt(c[0], &ok0);
							uint32_t c1 = ParseUInt(c[1], &ok1);
							bool ok2 = true, ok3 = true;
							uint32_t c2 = (c.size() > 2) ? ParseUInt(c[2], &ok2) : 0;
							uint32_t c3 = (c.size() > 3) ? ParseUInt(c[3], &ok3) : 0;
							if (ok0 && ok1 && ok2 && ok3 && c0 <= 255 && c1 <= 255 && c2 <= 255 && c3 <= 255)
							{
								std::array<uint8_t, 4> var = {
									static_cast<uint8_t>(c0),
									static_cast<uint8_t>(c1),
									static_cast<uint8_t>(c2),
									static_cast<uint8_t>(c3)
								};
								if (std::find(outConfig.colorVariations.begin(), outConfig.colorVariations.end(), var) == outConfig.colorVariations.end())
								{
									outConfig.colorVariations.push_back(var);
								}
							}
							else
							{
								logWarn("Invalid color IDs in variation '%s' (all values must be 0..255)", vStr.c_str());
							}
						}
						else
						{
							logWarn("Variation entry '%s' requires at least 2 color IDs", vStr.c_str());
						}
					}
				}
				else if (lowerKey.rfind("variation", 0) == 0)
				{
					std::vector<std::string> c = Split(val, ',');
					if (c.size() >= 2)
					{
						bool ok0 = false, ok1 = false;
						uint32_t c0 = ParseUInt(c[0], &ok0);
						uint32_t c1 = ParseUInt(c[1], &ok1);
						bool ok2 = true, ok3 = true;
						uint32_t c2 = (c.size() > 2) ? ParseUInt(c[2], &ok2) : 0;
						uint32_t c3 = (c.size() > 3) ? ParseUInt(c[3], &ok3) : 0;
						if (ok0 && ok1 && ok2 && ok3 && c0 <= 255 && c1 <= 255 && c2 <= 255 && c3 <= 255)
						{
							std::array<uint8_t, 4> var = {
								static_cast<uint8_t>(c0),
								static_cast<uint8_t>(c1),
								static_cast<uint8_t>(c2),
								static_cast<uint8_t>(c3)
							};
							if (std::find(outConfig.colorVariations.begin(), outConfig.colorVariations.end(), var) == outConfig.colorVariations.end())
							{
								outConfig.colorVariations.push_back(var);
							}
						}
						else
						{
							logWarn("Invalid color IDs in variation '%s' (all values must be 0..255)", val.c_str());
						}
					}
					else
					{
						logWarn("Variation '%s' requires at least 2 color IDs (e.g. '1, 1')", val.c_str());
					}
				}
			}
			else if (currentSection == "flags" || currentSection == "attrs")
			{
				outConfig.hasFlags = true;
				if (lowerKey == "hassiren" || lowerKey == "siren")
				{
					bool ok = false;
					bool b = ParseBool(val, &ok);
					if (ok)
						outConfig.hasSiren = b;
					else
						logWarn("Invalid hasSiren '%s' (expected true/false or 1/0)", val.c_str());
				}
				else if (lowerKey == "sirentype")
				{
					bool ok = false;
					int st = ParseInt(val, &ok);
					if (ok && st >= 0 && st <= 2)
						outConfig.sirenType = static_cast<int8_t>(st);
					else
						logWarn("Invalid sirenType '%s' (expected 0=none, 1=normal, 2=dual)", val.c_str());
				}
				else if (lowerKey == "hasbackfire" || lowerKey == "backfire")
				{
					bool ok = false;
					bool b = ParseBool(val, &ok);
					if (ok)
						outConfig.hasBackfire = b;
					else
						logWarn("Invalid hasBackfire '%s' (expected true/false or 1/0)", val.c_str());
				}
				else if (lowerKey == "hornsound" || lowerKey == "horn")
				{
					bool ok = false;
					int hs = ParseInt(val, &ok);
					if (ok && hs >= -1 && hs <= 20)
						outConfig.hornSound = static_cast<int8_t>(hs);
					else
						logWarn("Invalid hornSound '%s' (expected -1..20)", val.c_str());
				}
				else if (lowerKey == "hornpitch")
				{
					bool ok = false;
					float hp = ParseFloat(val, &ok);
					if (ok && hp >= 0.1f && hp <= 5.0f)
						outConfig.hornPitch = hp;
					else
						logWarn("Invalid hornPitch '%s' (expected range 0.1..5.0)", val.c_str());
				}
				else if (lowerKey == "lightingcategory" || lowerKey == "lightcategory")
				{
					bool ok = false;
					int lc = ParseInt(val, &ok);
					if (ok && lc >= -1 && lc <= 7)
						outConfig.lightingCategory = static_cast<int8_t>(lc);
					else
						logWarn("Invalid lightingCategory '%s' (expected -1..7)", val.c_str());
				}
				else if (lowerKey == "lightscale")
				{
					bool ok = false;
					float ls = ParseFloat(val, &ok);
					if (ok && ls >= 0.1f && ls <= 10.0f)
						outConfig.lightScale = ls;
					else
						logWarn("Invalid lightScale '%s' (expected range 0.1..10.0)", val.c_str());
				}
			}
			else if (currentSection == "lighting")
			{
				outConfig.hasLighting = true;
				bool ok = false;
				float f = ParseFloat(val, &ok);
				if (!ok || f < -10.0f || f > 10.0f)
				{
					logWarn("Invalid lighting coordinate/offset '%s' for key '%s' (expected range -10.0..10.0)", val.c_str(), key.c_str());
				}
				else
				{
					if (lowerKey == "headlightoffsetx") outConfig.headlightOffsetX = f;
					else if (lowerKey == "headlightoffsety") outConfig.headlightOffsetY = f;
					else if (lowerKey == "headlightoffsetz") outConfig.headlightOffsetZ = f;
					else if (lowerKey == "taillightoffsetx") outConfig.taillightOffsetX = f;
					else if (lowerKey == "taillightoffsety") outConfig.taillightOffsetY = f;
					else if (lowerKey == "taillightoffsetz") outConfig.taillightOffsetZ = f;
					else if (lowerKey == "headlightx") outConfig.headlightCustomX = f;
					else if (lowerKey == "headlighty") outConfig.headlightCustomY = f;
					else if (lowerKey == "headlightz") outConfig.headlightCustomZ = f;
					else if (lowerKey == "taillightx") outConfig.taillightCustomX = f;
					else if (lowerKey == "taillighty") outConfig.taillightCustomY = f;
					else if (lowerKey == "taillightz") outConfig.taillightCustomZ = f;
				}
			}
			else if (currentSection == "audio")
			{
				outConfig.hasAudio = true;
				auto isValidAudioFile = [](const std::string& filename) -> bool
				{
					std::string lower = ModelConfigParser::ToLower(filename);
					return (lower.size() > 4 && (lower.rfind(".wav") == lower.size() - 4 || lower.rfind(".ogg") == lower.size() - 4));
				};

				if (lowerKey == "engine")
				{
					if (isValidAudioFile(val))
						outConfig.engineFile = val;
					else
						logWarn("Ignoring invalid audio file '%s' for 'engine' (must be .wav or .ogg)", val.c_str());
				}
				else if (lowerKey == "acceleration" || lowerKey == "accel")
				{
					if (isValidAudioFile(val))
						outConfig.accelerationFile = val;
					else
						logWarn("Ignoring invalid audio file '%s' for 'acceleration' (must be .wav or .ogg)", val.c_str());
				}
				else if (lowerKey == "deacceleration" || lowerKey == "decel")
				{
					if (isValidAudioFile(val))
						outConfig.deaccelerationFile = val;
					else
						logWarn("Ignoring invalid audio file '%s' for 'deacceleration' (must be .wav or .ogg)", val.c_str());
				}
				else if (lowerKey == "brake")
				{
					if (isValidAudioFile(val))
						outConfig.brakeFile = val;
					else
						logWarn("Ignoring invalid audio file '%s' for 'brake' (must be .wav or .ogg)", val.c_str());
				}
				else if (lowerKey == "crash")
				{
					if (isValidAudioFile(val))
						outConfig.crashFile = val;
					else
						logWarn("Ignoring invalid audio file '%s' for 'crash' (must be .wav or .ogg)", val.c_str());
				}
				else if (lowerKey == "volume")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && f >= 0.0f && f <= 1.0f)
						outConfig.audioVolume = f;
					else
						logWarn("Invalid audio volume '%s' (expected range 0.0..1.0)", val.c_str());
				}
				else if (lowerKey == "mindistance" || lowerKey == "mindist")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && f >= 0.1f && f <= 500.0f)
						outConfig.audioMinDistance = f;
					else
						logWarn("Invalid audio minDistance '%s' (expected range 0.1..500.0)", val.c_str());
				}
				else if (lowerKey == "maxdistance" || lowerKey == "maxdist")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && f >= 1.0f && f <= 2000.0f)
						outConfig.audioMaxDistance = f;
					else
						logWarn("Invalid audio maxDistance '%s' (expected range 1.0..2000.0)", val.c_str());
				}
				else if (lowerKey == "pitchmultiplier" || lowerKey == "pitch")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && f >= 0.1f && f <= 4.0f)
						outConfig.audioPitchMultiplier = f;
					else
						logWarn("Invalid audio pitchMultiplier '%s' (expected range 0.1..4.0)", val.c_str());
				}
				else if (lowerKey == "accelpitchfactor")
				{
					bool ok = false;
					float f = ParseFloat(val, &ok);
					if (ok && f >= 0.0f && f <= 3.0f)
						outConfig.audioAccelPitchFactor = f;
					else
						logWarn("Invalid audio accelPitchFactor '%s' (expected range 0.0..3.0)", val.c_str());
				}
				else if (lowerKey == "mutenative")
				{
					bool ok = false;
					bool b = ParseBool(val, &ok);
					if (ok)
						outConfig.audioMuteNative = b ? 1 : 0;
					else
						logWarn("Invalid audio muteNative '%s' (expected true/false or 1/0)", val.c_str());
				}
			}
		}
		catch (const std::exception& ex)
		{
			logWarn("Unhandled exception while parsing key '%s': %s", key.c_str(), ex.what());
		}
		catch (...)
		{
			logWarn("Unknown exception while parsing key '%s'", key.c_str());
		}
	}

	if (core_)
	{
		core_->logLn(LogLevel::Message, "[ExtendedVeh] ModelConfigParser: Parsed '%s' for model '%s' (visualBase=%u, audioBase=%u, handlingBase=%u, ide=%d, handlingMods=%zu, carmods=%zu, colors=%zu)",
			sourceName.c_str(), outConfig.name.c_str(), outConfig.visualBase, outConfig.audioBase, outConfig.handlingBase,
			outConfig.hasIde ? 1 : 0, outConfig.handlingMods.size(), outConfig.modNames.size(), outConfig.colorVariations.size());
	}

	return true;
}

} // namespace HandlingMgr
