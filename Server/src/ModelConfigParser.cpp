#include "ModelConfigParser.h"
#include "CVehicleManager.hpp"
#include "HandlingDefault.h"
#include "HandlingManager.h"
#include "extendedveh.h"

#include <algorithm>
#include <cctype>
#include <charconv>
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

bool ModelConfigParser::ParseBool(const std::string& val)
{
	std::string lower = ToLower(val);
	return (lower == "1" || lower == "true" || lower == "yes" || lower == "on");
}

uint32_t ModelConfigParser::ParseUInt(const std::string& val)
{
	std::string s = val;
	Trim(s);
	if (s.empty())
		return 0;

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
		return static_cast<uint32_t>(std::stoul(s.substr(offset), nullptr, base));
	}
	catch (...)
	{
		return 0;
	}
}

float ModelConfigParser::ParseFloat(const std::string& val)
{
	std::string s = val;
	Trim(s);
	if (s.empty())
		return 0.0f;
	try
	{
		return std::stof(s);
	}
	catch (...)
	{
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
	return ParseString(buffer.str(), outConfig, fallbackBaseModel);
}

bool ModelConfigParser::ParseString(const std::string& content, ModelConfig& outConfig, uint32_t fallbackBaseModel)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;

	uint16_t baseModel = static_cast<uint16_t>(fallbackBaseModel);
	HandlingDefault::copyDefaultModelHandling(baseModel, &outConfig.handlingData);

	std::stringstream ss(content);
	std::string rawLine;
	std::string currentSection = "model";

	while (std::getline(ss, rawLine))
	{
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

		// Section dispatch
		if (currentSection == "model" || currentSection == "general")
		{
			if (lowerKey == "name" || lowerKey == "friendlyname")
			{
				outConfig.name = val;
			}
			else if (lowerKey == "visualbase" || lowerKey == "visualbasemodel")
			{
				int id = ResolveVehicleModelId(val);
				if (id > 0)
					outConfig.visualBase = static_cast<uint32_t>(id);
			}
			else if (lowerKey == "audiobase" || lowerKey == "audiobasemodel")
			{
				int id = ResolveVehicleModelId(val);
				if (id > 0)
					outConfig.audioBase = static_cast<uint32_t>(id);
			}
			else if (lowerKey == "handlingbase" || lowerKey == "handlingbasemodel")
			{
				int id = ResolveVehicleModelId(val);
				if (id > 0)
					outConfig.handlingBase = static_cast<uint32_t>(id);
			}
			else if (lowerKey == "engineonsound" || lowerKey == "soundon")
			{
				outConfig.engineOnSound = static_cast<int16_t>(std::stoi(val));
			}
			else if (lowerKey == "engineoffsound" || lowerKey == "soundoff")
			{
				outConfig.engineOffSound = static_cast<int16_t>(std::stoi(val));
			}
			else if (lowerKey == "acceleratesound")
			{
				outConfig.accelerateSound = static_cast<int16_t>(std::stoi(val));
			}
			else if (lowerKey == "deceleratesound")
			{
				outConfig.decelerateSound = static_cast<int16_t>(std::stoi(val));
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
				outConfig.frequency = static_cast<uint16_t>(ParseUInt(val));
			}
			else if (lowerKey == "level")
			{
				outConfig.level = static_cast<uint8_t>(ParseUInt(val));
			}
			else if (lowerKey == "comprules" || lowerKey == "comprate")
			{
				outConfig.compRules = static_cast<uint8_t>(ParseUInt(val));
			}
			else if (lowerKey == "wheelmodelid" || lowerKey == "wheelmodel" || lowerKey == "wheelid")
			{
				outConfig.wheelModelId = static_cast<int16_t>(std::stoi(val));
			}
			else if (lowerKey == "wheelscalefront" || lowerKey == "frontwheelscale")
			{
				outConfig.wheelScaleFront = ParseFloat(val);
			}
			else if (lowerKey == "wheelscalerear" || lowerKey == "rearwheelscale")
			{
				outConfig.wheelScaleRear = ParseFloat(val);
			}
			else if (lowerKey == "wheelscale")
			{
				float s = ParseFloat(val);
				outConfig.wheelScaleFront = s;
				outConfig.wheelScaleRear = s;
			}
			else if (lowerKey == "wheelupgradeclass")
			{
				outConfig.wheelUpgradeClass = static_cast<uint8_t>(ParseUInt(val));
			}
			else if (lowerKey == "numextras")
			{
				outConfig.numExtras = static_cast<uint8_t>(ParseUInt(val));
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
				outConfig.handlingData.m_fMass = ParseFloat(val);
				recordMod(HANDL_FMASS, TYPE_FLOAT, outConfig.handlingData.m_fMass);
			}
			else if (lowerKey == "turnmass")
			{
				outConfig.handlingData.m_fTurnMass = ParseFloat(val);
				recordMod(HANDL_FTURNMASS, TYPE_FLOAT, outConfig.handlingData.m_fTurnMass);
			}
			else if (lowerKey == "dragmult" || lowerKey == "dragmultiplier")
			{
				outConfig.handlingData.m_fDragMult = ParseFloat(val);
				recordMod(HANDL_FDRAGMULTIPLIER, TYPE_FLOAT, outConfig.handlingData.m_fDragMult);
			}
			else if (lowerKey == "centreofmassx" || lowerKey == "centerofmassx" || lowerKey == "comx")
			{
				outConfig.handlingData.m_vecCentreOfMass.fX = ParseFloat(val);
				recordMod(HANDL_CENTREOFMASS_X, TYPE_FLOAT, outConfig.handlingData.m_vecCentreOfMass.fX);
			}
			else if (lowerKey == "centreofmassy" || lowerKey == "centerofmassy" || lowerKey == "comy")
			{
				outConfig.handlingData.m_vecCentreOfMass.fY = ParseFloat(val);
				recordMod(HANDL_CENTREOFMASS_Y, TYPE_FLOAT, outConfig.handlingData.m_vecCentreOfMass.fY);
			}
			else if (lowerKey == "centreofmassz" || lowerKey == "centerofmassz" || lowerKey == "comz")
			{
				outConfig.handlingData.m_vecCentreOfMass.fZ = ParseFloat(val);
				recordMod(HANDL_CENTREOFMASS_Z, TYPE_FLOAT, outConfig.handlingData.m_vecCentreOfMass.fZ);
			}
			else if (lowerKey == "percentsubmerged" || lowerKey == "submerged")
			{
				outConfig.handlingData.m_nPercentSubmerged = static_cast<uint8_t>(ParseUInt(val));
				recordMod(HANDL_NPERCENTSUBMERGED, TYPE_BYTE, outConfig.handlingData.m_nPercentSubmerged);
			}
			else if (lowerKey == "tractionmultiplier" || lowerKey == "tractionmult")
			{
				outConfig.handlingData.m_fTractionMultiplier = ParseFloat(val);
				recordMod(HANDL_FTRACTIONMULTIPLIER, TYPE_FLOAT, outConfig.handlingData.m_fTractionMultiplier);
			}
			else if (lowerKey == "tractionloss")
			{
				outConfig.handlingData.m_fTractionLoss = ParseFloat(val);
				recordMod(HANDL_FTRACTIONLOSS, TYPE_FLOAT, outConfig.handlingData.m_fTractionLoss);
			}
			else if (lowerKey == "tractionbias")
			{
				outConfig.handlingData.m_fTractionBias = ParseFloat(val);
				recordMod(HANDL_FTRACTIONBIAS, TYPE_FLOAT, outConfig.handlingData.m_fTractionBias);
			}
			else if (lowerKey == "numberofgears" || lowerKey == "gears")
			{
				outConfig.handlingData.m_transmissionData.m_nNumberOfGears = static_cast<unsigned char>(ParseUInt(val));
				recordMod(HANDL_TR_NNUMBEROFGEARS, TYPE_BYTE, outConfig.handlingData.m_transmissionData.m_nNumberOfGears);
			}
			else if (lowerKey == "maxvelocity" || lowerKey == "topspeed")
			{
				outConfig.handlingData.m_transmissionData.m_fMaxGearVelocity = ParseFloat(val);
				recordMod(HANDL_TR_FMAXVELOCITY, TYPE_FLOAT, outConfig.handlingData.m_transmissionData.m_fMaxGearVelocity);
			}
			else if (lowerKey == "engineacceleration" || lowerKey == "acceleration")
			{
				outConfig.handlingData.m_transmissionData.m_fEngineAcceleration = ParseFloat(val);
				recordMod(HANDL_TR_FENGINEACCELERATION, TYPE_FLOAT, outConfig.handlingData.m_transmissionData.m_fEngineAcceleration);
			}
			else if (lowerKey == "engineinertia" || lowerKey == "inertia")
			{
				outConfig.handlingData.m_transmissionData.m_fEngineInertia = ParseFloat(val);
				recordMod(HANDL_TR_FENGINEINERTIA, TYPE_FLOAT, outConfig.handlingData.m_transmissionData.m_fEngineInertia);
			}
			else if (lowerKey == "drivetype")
			{
				char dt = val.empty() ? 'R' : static_cast<char>(std::toupper(val[0]));
				outConfig.handlingData.m_transmissionData.m_nDriveType = static_cast<unsigned char>(dt);
				recordMod(HANDL_TR_NDRIVETYPE, TYPE_BYTE, outConfig.handlingData.m_transmissionData.m_nDriveType);
			}
			else if (lowerKey == "enginetype")
			{
				char et = val.empty() ? 'P' : static_cast<char>(std::toupper(val[0]));
				outConfig.handlingData.m_transmissionData.m_nEngineType = static_cast<unsigned char>(et);
				recordMod(HANDL_TR_NENGINETYPE, TYPE_BYTE, outConfig.handlingData.m_transmissionData.m_nEngineType);
			}
			else if (lowerKey == "brakedeceleration")
			{
				outConfig.handlingData.m_fBrakeDeceleration = ParseFloat(val);
				recordMod(HANDL_FBRAKEDECELERATION, TYPE_FLOAT, outConfig.handlingData.m_fBrakeDeceleration);
			}
			else if (lowerKey == "brakebias")
			{
				outConfig.handlingData.m_fBrakeBias = ParseFloat(val);
				recordMod(HANDL_FBRAKEBIAS, TYPE_FLOAT, outConfig.handlingData.m_fBrakeBias);
			}
			else if (lowerKey == "abs")
			{
				outConfig.handlingData.m_bABS = ParseBool(val) ? 1 : 0;
				recordMod(HANDL_BABS, TYPE_BYTE, static_cast<uint8_t>(outConfig.handlingData.m_bABS));
			}
			else if (lowerKey == "steeringlock")
			{
				outConfig.handlingData.m_fSteeringLock = ParseFloat(val);
				recordMod(HANDL_FSTEERINGLOCK, TYPE_FLOAT, outConfig.handlingData.m_fSteeringLock);
			}
			else if (lowerKey == "suspensionforcelevel")
			{
				outConfig.handlingData.m_fSuspensionForceLevel = ParseFloat(val);
				recordMod(HANDL_FSUSPENSIONFORCELEVEL, TYPE_FLOAT, outConfig.handlingData.m_fSuspensionForceLevel);
			}
			else if (lowerKey == "suspensiondampinglevel")
			{
				outConfig.handlingData.m_fSuspensionDampingLevel = ParseFloat(val);
				recordMod(HANDL_FSUSPENSIONDAMPINGLEVEL, TYPE_FLOAT, outConfig.handlingData.m_fSuspensionDampingLevel);
			}
			else if (lowerKey == "suspensionhighspdcomdamp")
			{
				outConfig.handlingData.m_fSuspensionHighSpdComDamp = ParseFloat(val);
				recordMod(HANDL_FSUSPENSIONHIGHSPDCOMDAMP, TYPE_FLOAT, outConfig.handlingData.m_fSuspensionHighSpdComDamp);
			}
			else if (lowerKey == "suspensionupperlimit")
			{
				outConfig.handlingData.m_fSuspensionUpperLimit = ParseFloat(val);
				recordMod(HANDL_FSUSPENSIONUPPERLIMIT, TYPE_FLOAT, outConfig.handlingData.m_fSuspensionUpperLimit);
			}
			else if (lowerKey == "suspensionlowerlimit")
			{
				outConfig.handlingData.m_fSuspensionLowerLimit = ParseFloat(val);
				recordMod(HANDL_FSUSPENSIONLOWERLIMIT, TYPE_FLOAT, outConfig.handlingData.m_fSuspensionLowerLimit);
			}
			else if (lowerKey == "suspensionbias")
			{
				outConfig.handlingData.m_fSuspensionBiasBetweenFrontAndRear = ParseFloat(val);
				recordMod(HANDL_FSUSPENSIONBIAS, TYPE_FLOAT, outConfig.handlingData.m_fSuspensionBiasBetweenFrontAndRear);
			}
			else if (lowerKey == "suspensionantidivemultiplier")
			{
				outConfig.handlingData.m_fSuspensionAntiDiveMultiplier = ParseFloat(val);
				recordMod(HANDL_FSUSPENSIONANTIDIVEMULT, TYPE_FLOAT, outConfig.handlingData.m_fSuspensionAntiDiveMultiplier);
			}
			else if (lowerKey == "seatoffsetdistance")
			{
				outConfig.handlingData.m_fSeatOffsetDistance = ParseFloat(val);
				recordMod(HANDL_FSEATOFFSETDISTANCE, TYPE_FLOAT, outConfig.handlingData.m_fSeatOffsetDistance);
			}
			else if (lowerKey == "collisiondamagemultiplier")
			{
				outConfig.handlingData.m_fCollisionDamageMultiplier = ParseFloat(val);
				recordMod(HANDL_FCOLLISIONDAMAGEMULT, TYPE_FLOAT, outConfig.handlingData.m_fCollisionDamageMultiplier);
			}
			else if (lowerKey == "monetaryvalue")
			{
				outConfig.handlingData.m_nMonetaryValue = ParseUInt(val);
				recordMod(HANDL_UIMONETARYVALUE, TYPE_UINT, outConfig.handlingData.m_nMonetaryValue);
			}
			else if (lowerKey == "modelflags")
			{
				outConfig.handlingData.m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(ParseUInt(val));
				recordMod(HANDL_MODELFLAGS, TYPE_FLAG, static_cast<unsigned int>(outConfig.handlingData.m_nModelFlags));
			}
			else if (lowerKey == "handlingflags")
			{
				outConfig.handlingData.m_nHandlingFlags = static_cast<eVehicleHandlingFlags>(ParseUInt(val));
				recordMod(HANDL_HANDLINGFLAGS, TYPE_FLAG, static_cast<unsigned int>(outConfig.handlingData.m_nHandlingFlags));
			}
			else if (lowerKey == "frontlights" || lowerKey == "frontlight")
			{
				outConfig.handlingData.m_nFrontLights = static_cast<eVehicleLightsSize>(ParseUInt(val));
				recordMod(HANDL_FRONTLIGHTS, TYPE_BYTE, static_cast<uint8_t>(outConfig.handlingData.m_nFrontLights));
			}
			else if (lowerKey == "rearlights" || lowerKey == "rearlight")
			{
				outConfig.handlingData.m_nRearLights = static_cast<eVehicleLightsSize>(ParseUInt(val));
				recordMod(HANDL_REARLIGHTS, TYPE_BYTE, static_cast<uint8_t>(outConfig.handlingData.m_nRearLights));
			}
			else if (lowerKey == "animgroup")
			{
				outConfig.handlingData.m_nAnimGroup = static_cast<unsigned char>(ParseUInt(val));
				recordMod(HANDL_ANIMGROUP, TYPE_BYTE, outConfig.handlingData.m_nAnimGroup);
			}
		}
		else if (currentSection == "carmods" || currentSection == "mods")
		{
			outConfig.hasCarmods = true;
			std::vector<std::string> parts = Split(val, ',');
			if (parts.empty())
				parts = Split(val, ' ');

			for (const auto& part : parts)
			{
				if (std::find(outConfig.modNames.begin(), outConfig.modNames.end(), part) == outConfig.modNames.end())
				{
					outConfig.modNames.push_back(part);
				}
				int compId = ResolveUpgradeComponentId(part);
				if (compId > 0 && std::find(outConfig.modIds.begin(), outConfig.modIds.end(), compId) == outConfig.modIds.end())
				{
					outConfig.modIds.push_back(compId);
				}
			}
			outConfig.categorizedMods[lowerKey] = parts;
		}
		else if (currentSection == "carcols" || currentSection == "colors")
		{
			outConfig.hasCarcols = true;
			if (lowerKey == "primary")
			{
				outConfig.defaultPrimaryColor = static_cast<uint8_t>(ParseUInt(val));
			}
			else if (lowerKey == "secondary")
			{
				outConfig.defaultSecondaryColor = static_cast<uint8_t>(ParseUInt(val));
			}
			else if (lowerKey == "tertiary")
			{
				outConfig.defaultTertiaryColor = static_cast<uint8_t>(ParseUInt(val));
			}
			else if (lowerKey == "quaternary")
			{
				outConfig.defaultQuaternaryColor = static_cast<uint8_t>(ParseUInt(val));
			}
			else if (lowerKey.rfind("variation", 0) == 0)
			{
				std::vector<std::string> c = Split(val, ',');
				if (c.size() >= 2)
				{
					std::array<uint8_t, 4> var = {
						static_cast<uint8_t>(ParseUInt(c[0])),
						static_cast<uint8_t>(ParseUInt(c[1])),
						static_cast<uint8_t>(c.size() > 2 ? ParseUInt(c[2]) : 0),
						static_cast<uint8_t>(c.size() > 3 ? ParseUInt(c[3]) : 0)
					};
					outConfig.colorVariations.push_back(var);
				}
			}
			else if (lowerKey == "variations")
			{
				std::vector<std::string> varList = Split(val, ';');
				for (const auto& vStr : varList)
				{
					std::vector<std::string> c = Split(vStr, ',');
					if (c.size() >= 2)
					{
						std::array<uint8_t, 4> var = {
							static_cast<uint8_t>(ParseUInt(c[0])),
							static_cast<uint8_t>(ParseUInt(c[1])),
							static_cast<uint8_t>(c.size() > 2 ? ParseUInt(c[2]) : 0),
							static_cast<uint8_t>(c.size() > 3 ? ParseUInt(c[3]) : 0)
						};
						outConfig.colorVariations.push_back(var);
					}
				}
			}
		}
		else if (currentSection == "flags" || currentSection == "attrs")
		{
			outConfig.hasFlags = true;
			if (lowerKey == "hassiren" || lowerKey == "siren")
			{
				outConfig.hasSiren = ParseBool(val);
			}
			else if (lowerKey == "sirentype")
			{
				outConfig.sirenType = static_cast<int8_t>(std::stoi(val));
			}
			else if (lowerKey == "hasbackfire" || lowerKey == "backfire")
			{
				outConfig.hasBackfire = ParseBool(val);
			}
			else if (lowerKey == "hornsound" || lowerKey == "horn")
			{
				outConfig.hornSound = static_cast<int8_t>(std::stoi(val));
			}
			else if (lowerKey == "hornpitch")
			{
				outConfig.hornPitch = ParseFloat(val);
			}
			else if (lowerKey == "lightingcategory" || lowerKey == "lightcategory")
			{
				outConfig.lightingCategory = static_cast<int8_t>(std::stoi(val));
			}
			else if (lowerKey == "lightscale")
			{
				outConfig.lightScale = ParseFloat(val);
			}
		}
		else if (currentSection == "lighting")
		{
			outConfig.hasLighting = true;
			if (lowerKey == "headlightoffsetx")
			{
				outConfig.headlightOffsetX = ParseFloat(val);
			}
			else if (lowerKey == "headlightoffsety")
			{
				outConfig.headlightOffsetY = ParseFloat(val);
			}
			else if (lowerKey == "headlightoffsetz")
			{
				outConfig.headlightOffsetZ = ParseFloat(val);
			}
			else if (lowerKey == "taillightoffsetx")
			{
				outConfig.taillightOffsetX = ParseFloat(val);
			}
			else if (lowerKey == "taillightoffsety")
			{
				outConfig.taillightOffsetY = ParseFloat(val);
			}
			else if (lowerKey == "taillightoffsetz")
			{
				outConfig.taillightOffsetZ = ParseFloat(val);
			}
			else if (lowerKey == "headlightx")
			{
				outConfig.headlightCustomX = ParseFloat(val);
			}
			else if (lowerKey == "headlighty")
			{
				outConfig.headlightCustomY = ParseFloat(val);
			}
			else if (lowerKey == "headlightz")
			{
				outConfig.headlightCustomZ = ParseFloat(val);
			}
			else if (lowerKey == "taillightx")
			{
				outConfig.taillightCustomX = ParseFloat(val);
			}
			else if (lowerKey == "taillighty")
			{
				outConfig.taillightCustomY = ParseFloat(val);
			}
			else if (lowerKey == "taillightz")
			{
				outConfig.taillightCustomZ = ParseFloat(val);
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
				else if (core_)
					core_->logLn(LogLevel::Warning, "[ExtendedVeh] ModelConfigParser: Ignoring invalid audio file '%s' for 'engine' (must be .wav or .ogg)", val.c_str());
			}
			else if (lowerKey == "acceleration" || lowerKey == "accel")
			{
				if (isValidAudioFile(val))
					outConfig.accelerationFile = val;
				else if (core_)
					core_->logLn(LogLevel::Warning, "[ExtendedVeh] ModelConfigParser: Ignoring invalid audio file '%s' for 'acceleration' (must be .wav or .ogg)", val.c_str());
			}
			else if (lowerKey == "deacceleration" || lowerKey == "decel")
			{
				if (isValidAudioFile(val))
					outConfig.deaccelerationFile = val;
				else if (core_)
					core_->logLn(LogLevel::Warning, "[ExtendedVeh] ModelConfigParser: Ignoring invalid audio file '%s' for 'deacceleration' (must be .wav or .ogg)", val.c_str());
			}
			else if (lowerKey == "brake")
			{
				if (isValidAudioFile(val))
					outConfig.brakeFile = val;
				else if (core_)
					core_->logLn(LogLevel::Warning, "[ExtendedVeh] ModelConfigParser: Ignoring invalid audio file '%s' for 'brake' (must be .wav or .ogg)", val.c_str());
			}
			else if (lowerKey == "crash")
			{
				if (isValidAudioFile(val))
					outConfig.crashFile = val;
				else if (core_)
					core_->logLn(LogLevel::Warning, "[ExtendedVeh] ModelConfigParser: Ignoring invalid audio file '%s' for 'crash' (must be .wav or .ogg)", val.c_str());
			}
			else if (lowerKey == "volume")
			{
				outConfig.audioVolume = std::clamp(ParseFloat(val), 0.0f, 1.0f);
			}
			else if (lowerKey == "mindistance" || lowerKey == "mindist")
			{
				outConfig.audioMinDistance = std::max(0.1f, ParseFloat(val));
			}
			else if (lowerKey == "maxdistance" || lowerKey == "maxdist")
			{
				outConfig.audioMaxDistance = std::max(1.0f, ParseFloat(val));
			}
			else if (lowerKey == "pitchmultiplier" || lowerKey == "pitch")
			{
				outConfig.audioPitchMultiplier = std::clamp(ParseFloat(val), 0.1f, 4.0f);
			}
			else if (lowerKey == "accelpitchfactor")
			{
				outConfig.audioAccelPitchFactor = std::clamp(ParseFloat(val), 0.0f, 3.0f);
			}
			else if (lowerKey == "mutenative")
			{
				outConfig.audioMuteNative = ParseBool(val) ? 1 : 0;
			}
		}
	}

	if (core_)
	{
		core_->logLn(LogLevel::Message, "[ExtendedVeh] ModelConfigParser: Parsed model '%s' (visualBase=%u, audioBase=%u, handlingBase=%u, ide=%d, handlingMods=%zu, carmods=%zu, colors=%zu)",
			outConfig.name.c_str(), outConfig.visualBase, outConfig.audioBase, outConfig.handlingBase,
			outConfig.hasIde ? 1 : 0, outConfig.handlingMods.size(), outConfig.modNames.size(), outConfig.colorVariations.size());
	}

	return true;
}

} // namespace HandlingMgr
