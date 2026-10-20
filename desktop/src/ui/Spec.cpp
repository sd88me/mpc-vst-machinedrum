#include "Spec.h"

namespace mm::uispec {

const Param kAmpFxPage[8] = {dial("AMD"), dial("AMF"), dial("EQF", 64), bipolar("EQG"), dial("FLTF"), dial("FLTW", 127), dial("FLTQ"), dial("SRR")};
const Param kRoutePage[8] = {dial("DIST"), dial("VOL", 100), bipolar("PAN"), dial("DEL"), dial("REV"), dial("LFOS"), dial("LFOD"), dial("LFOM")};
const char* const kTrackNames[16] = {"01-BD", "02-SD", "03-HT", "04-MT", "05-LT", "06-CP", "07-RS", "08-CB", "09-CH", "10-OH", "11-RC", "12-CC", "13-M1", "14-M2", "15-M3", "16-M4"};
const char* const kUpdateNames[3] = {"FREE", "TRIG", "HOLD"};
const char* const kFixedParamNames[16] = {"AMD", "AMF", "EQF", "EQG", "FLTF", "FLTW", "FLTQ", "SRR", "DIST", "VOL", "PAN", "DEL", "REV", "LFOS", "LFOD", "LFOM"};
const Param kLfoPage[8] = {
	{"TRACK", Display::Text, 0, 15, kTrackNames},
	{"PARAM", Display::Text, 0, 23, nullptr},
	{"SHP1", Display::Shape, 0, 5, nullptr},
	{"SHP2", Display::Shape, 0, 5, nullptr},
	{"UPDTE", Display::Text, 0, 2, kUpdateNames},
	dial("SPEED"), dial("DEPTH"), dial("SHMIX")};

const char* const kPairParamNames[24] = {"SYN1", "SYN2", "SYN3", "SYN4", "SYN5", "SYN6", "SYN7", "SYN8", "AMD", "AMF", "EQF", "EQG", "FLTF", "FLTW", "FLTQ", "SRR",
	"DIST", "VOL", "PAN", "DEL", "REV", "LFOS", "LFOD", "LFOM"};
#define PAIR(n) {"TRK" #n, Display::Text, 0, 15, kTrackNames}, {"PRM" #n, Display::Text, 0, 23, kPairParamNames}
const Param kCtr8Pages[2][8] = {{PAIR(1), PAIR(2), PAIR(3), PAIR(4)}, {PAIR(5), PAIR(6), PAIR(7), PAIR(8)}};
#undef PAIR
const char* const kCtr8PageNames[2] = {"TARGETS 1-4", "TARGETS 5-8"};

const char* const kMasterPageNames[4] = {"REVERB", "ECHO", "EQ", "DYNAMIX"};
const Param kMasterPages[4][8] = {
	{dial("REV1"), dial("REV2"), dial("REV3"), dial("REV4"), dial("REV5"), dial("REV6"), dial("REV7"), dial("REV8")},
	{dial("ECH1"), dial("ECH2"), dial("ECH3"), dial("ECH4"), dial("ECH5"), dial("ECH6"), dial("ECH7"), dial("ECH8")},
	{dial("EQ1"), dial("EQ2"), dial("EQ3"), dial("EQ4"), dial("EQ5"), dial("EQ6"), dial("EQ7"), dial("EQ8")},
	{dial("DYN1"), dial("DYN2"), dial("DYN3"), dial("DYN4"), dial("DYN5"), dial("DYN6"), dial("DYN7"), dial("DYN8")}};

}	// namespace mm::uispec
