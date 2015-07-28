#ifndef VALHALLA_ODIN_NARRATIVEBUILDER_H_
#define VALHALLA_ODIN_NARRATIVEBUILDER_H_

#include <vector>

#include <valhalla/proto/trippath.pb.h>
#include <valhalla/proto/directions_options.pb.h>

#include <valhalla/odin/maneuver.h>

namespace valhalla {
namespace odin {

const bool kLimitByConseuctiveCount = true;
constexpr uint32_t kElementMaxCount = 4;
constexpr uint32_t kVerbalAlertElementMaxCount = 1;
constexpr uint32_t kVerbalPreElementMaxCount = 2;
constexpr uint32_t kVerbalPostElementMaxCount = 2;
constexpr float kVerbalPostMinimumRampLength = 2.0f;  // Kilometers
const std::string kVerbalDelim = ", ";

class NarrativeBuilder {
 public:

  static void Build(const DirectionsOptions& directions_options,
                    std::list<Maneuver>& maneuvers);

 protected:
  NarrativeBuilder();

  static std::string FormStartInstruction(Maneuver& maneuver);

  static std::string FormVerbalStartInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalPreElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormDestinationInstruction(Maneuver& maneuver);

  static std::string FormVerbalAlertDestinationInstruction(Maneuver& maneuver);

  static std::string FormVerbalDestinationInstruction(Maneuver& maneuver);

  static std::string FormBecomesInstruction(Maneuver& maneuver,
                                            Maneuver* prev_maneuver);

  static std::string FormVerbalBecomesInstruction(
      Maneuver& maneuver, Maneuver* prev_maneuver,
      uint32_t element_max_count = kVerbalPreElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormContinueInstruction(Maneuver& maneuver);

  static std::string FormVerbalAlertContinueInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalAlertElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormVerbalContinueInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalPreElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormTurnInstruction(Maneuver& maneuver);

  static std::string FormVerbalAlertTurnInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalAlertElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormVerbalTurnInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalPreElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormTurnToStayOnInstruction(Maneuver& maneuver);

  static std::string FormVerbalAlertTurnToStayOnInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalAlertElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormVerbalTurnToStayOnInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalPreElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormBearInstruction(Maneuver& maneuver);

  static std::string FormVerbalAlertBearInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalAlertElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormVerbalBearInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalPreElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormBearToStayOnInstruction(Maneuver& maneuver);

  static std::string FormVerbalAlertBearToStayOnInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalAlertElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormVerbalBearToStayOnInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalPreElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormUturnInstruction(Maneuver& maneuver);

  static std::string FormVerbalAlertUturnInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalAlertElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormVerbalUturnInstruction(
      Maneuver& maneuver,
      uint32_t element_max_count = kVerbalPreElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormRampStraightInstruction(
      Maneuver& maneuver,
      bool limit_by_consecutive_count = kLimitByConseuctiveCount,
      uint32_t element_max_count = kElementMaxCount);

  static std::string FormVerbalAlertRampStraightInstruction(
      Maneuver& maneuver,
      bool limit_by_consecutive_count = kLimitByConseuctiveCount,
      uint32_t element_max_count = kVerbalAlertElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormVerbalRampStraightInstruction(
      Maneuver& maneuver,
      bool limit_by_consecutive_count = kLimitByConseuctiveCount,
      uint32_t element_max_count = kVerbalPreElementMaxCount,
      std::string delim = kVerbalDelim);

  static void FormRampRightInstruction(Maneuver& maneuver);

  static void FormRampLeftInstruction(Maneuver& maneuver);

  static void FormExitRightInstruction(Maneuver& maneuver);

  static void FormExitLeftInstruction(Maneuver& maneuver);

  static void FormStayStraightInstruction(Maneuver& maneuver);

  static void FormStayRightInstruction(Maneuver& maneuver);

  static void FormStayLeftInstruction(Maneuver& maneuver);

  static void FormStayStraightToStayOnInstruction(Maneuver& maneuver);

  static void FormStayRightToStayOnInstruction(Maneuver& maneuver);

  static void FormStayLeftToStayOnInstruction(Maneuver& maneuver);

  static void FormMergeInstruction(Maneuver& maneuver);

  static void FormEnterRoundaboutInstruction(Maneuver& maneuver);

  static void FormExitRoundaboutInstruction(Maneuver& maneuver);

  static void FormEnterFerryInstruction(Maneuver& maneuver);

  static void FormExitFerryInstruction(Maneuver& maneuver);

  static void FormTransitConnectionStartInstruction(Maneuver& maneuver);

  static void FormTransitConnectionTransferInstruction(Maneuver& maneuver);

  static void FormTransitConnectionDestinationInstruction(Maneuver& maneuver);

  static void FormTransitInstruction(Maneuver& maneuver);

  static void FormTransitRemainOnInstruction(Maneuver& maneuver);

  static void FormTransitTransferInstruction(Maneuver& maneuver);

  static void FormPostTransitConnectionDestinationInstruction(Maneuver& maneuver);

  static std::string FormVerbalPostTransitionInstruction(
      Maneuver& maneuver, DirectionsOptions_Units units,
      bool include_street_names = false,
      uint32_t element_max_count = kVerbalPostElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormVerbalPostTransitionKilometersInstruction(
      Maneuver& maneuver, bool include_street_names = false,
      uint32_t element_max_count = kVerbalPostElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormVerbalPostTransitionMilesInstruction(
      Maneuver& maneuver, bool include_street_names = false,
      uint32_t element_max_count = kVerbalPostElementMaxCount,
      std::string delim = kVerbalDelim);

  static std::string FormCardinalDirection(
      TripDirections_Maneuver_CardinalDirection cardinal_direction);

  static std::string FormTurnTypeInstruction(TripDirections_Maneuver_Type type);

  static std::string FormBearTypeInstruction(TripDirections_Maneuver_Type type);

  static std::string FormOrdinalValue(uint32_t value);

};

}
}

#endif  // VALHALLA_ODIN_NARRATIVEBUILDER_H_
