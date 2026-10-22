#include "trip_history_view.h"

#include <cstdio>
#include <cstring>

constexpr size_t TripHistoryView::kMaxRecentTrips;
constexpr uint32_t TripHistoryView::kIdleTimeoutMs;

namespace {
bool useRussian(const ConfigData& config) {
  return config.uiLanguage() == UiLanguage::Russian;
}

void formatCompactDuration(uint32_t seconds, bool ru, char* out, size_t cap) {
  const uint32_t totalMinutes = (seconds + 30U) / 60U;
  if (totalMinutes < 60U) {
    std::snprintf(out, cap, ru ? "%u М" : "%uM",
                  static_cast<unsigned>(totalMinutes));
    return;
  }
  const uint32_t hours = totalMinutes / 60U;
  const uint32_t mins = totalMinutes % 60U;
  std::snprintf(out, cap, ru ? "%uЧ %02uМ" : "%uH %02uM",
                static_cast<unsigned>(hours), static_cast<unsigned>(mins));
}

void extractDayMonth(const char isoDate[11], char outDdMm[6]) {
  // isoDate is "YYYY-MM-DD"
  if (isoDate[0] != '\0' && std::strlen(isoDate) >= 10) {
    outDdMm[0] = isoDate[8];
    outDdMm[1] = isoDate[9];
    outDdMm[2] = '.';
    outDdMm[3] = isoDate[5];
    outDdMm[4] = isoDate[6];
    outDdMm[5] = '\0';
  } else {
    std::snprintf(outDdMm, 6, "--.--");
  }
}
}  // namespace

void TripHistoryView::syncIfNeeded(const TripHistory& history) {
  if (syncedGeneration_ != history.generation()) {
    reload(history);
  }
}

void TripHistoryView::reload(const TripHistory& history) {
  recentCount_ = 0;
  hasLatestDay_ = false;
  latestDay_ = TripDaySummary{};
  history.readRecentAndLatestDay(kMaxRecentTrips, recent_, recentCount_,
                                 hasLatestDay_, latestDay_);
  syncedGeneration_ = history.generation();
  const size_t cards = totalCards();
  if (cards == 0U) {
    cursor_ = 0;
  } else if (cursor_ >= cards) {
    cursor_ = cards - 1U;
  }
}

void TripHistoryView::open(uint32_t nowMs) {
  active_ = true;
  cursor_ = 0;
  lastInteractionAtMs_ = nowMs;
}

void TripHistoryView::close() {
  active_ = false;
  cursor_ = 0;
}

bool TripHistoryView::tick(uint32_t nowMs) {
  if (!active_) return false;
  if (nowMs - lastInteractionAtMs_ >= kIdleTimeoutMs) {
    close();
    return true;
  }
  return false;
}

void TripHistoryView::stepNext(uint32_t nowMs) {
  if (!active_) return;
  const size_t cards = totalCards();
  if (cards == 0U) {
    close();
    return;
  }
  cursor_ = (cursor_ + 1U) % cards;
  lastInteractionAtMs_ = nowMs;
}

TripHistoryCardText TripHistoryView::buildCard(const ConfigData& config,
                                               uint32_t currentEpochSec) const {
  const bool ru = useRussian(config);
  TripHistoryCardText card{};
  std::snprintf(card.title, sizeof(card.title), "%s",
                ru ? "ИСТОРИЯ" : "HISTORY");
  std::snprintf(card.leftLabel, sizeof(card.leftLabel), "%s",
                ru ? "БЕНЗИН" : "PETROL");
  std::snprintf(card.rightLabel, sizeof(card.rightLabel), "%s",
                ru ? "ГАЗ" : "LPG");

  const size_t cards = totalCards();
  if (cards == 0U) {
    card.empty = true;
    std::snprintf(card.subtitle, sizeof(card.subtitle), "%s",
                  ru ? "НЕТ ЗАКРЫТЫХ ПОЕЗДОК" : "NO CLOSED TRIPS");
    std::snprintf(card.centerValue, sizeof(card.centerValue), "0.0");
    std::snprintf(card.centerUnit, sizeof(card.centerUnit), "%s",
                  ru ? "КМ | АРХИВ ПУСТ" : "KM | EMPTY");
    std::snprintf(card.leftValue, sizeof(card.leftValue),
                  ru ? "0.00 л" : "0.00 L");
    std::snprintf(card.rightValue, sizeof(card.rightValue),
                  ru ? "0.00 л" : "0.00 L");
    std::snprintf(card.footer, sizeof(card.footer), "%s",
                  ru ? "КНОПКА - ВЫХОД" : "PRESS TO EXIT");
    return card;
  }

  card.empty = false;
  const size_t effectiveCursor = cursor_ < cards ? cursor_ : 0U;

  if (hasLatestDay_ && effectiveCursor == 0U) {
    card.isDaySummary = true;
    char ddmm[6];
    extractDayMonth(latestDay_.date, ddmm);

    bool isToday = false;
    if (currentEpochSec >= TripClock::kMinPlausibleEpochSec) {
      char todayIso[11] = {};
      formatLocalDateIso(
          epochToLocalDateTime(currentEpochSec, config.utcOffsetMin), todayIso);
      isToday = std::strcmp(latestDay_.date, todayIso) == 0;
    }

    if (isToday) {
      std::snprintf(card.subtitle, sizeof(card.subtitle),
                    ru ? "СЕГОДНЯ %s | %u П." : "TODAY %s | %u TR", ddmm,
                    static_cast<unsigned>(latestDay_.tripCount));
    } else {
      std::snprintf(card.subtitle, sizeof(card.subtitle),
                    ru ? "СУТКИ %s | %u П." : "DAY %s | %u TR", ddmm,
                    static_cast<unsigned>(latestDay_.tripCount));
    }

    std::snprintf(card.centerValue, sizeof(card.centerValue), "%.1f",
                  latestDay_.distanceKm);
    if (latestDay_.hasAverageL100) {
      std::snprintf(card.centerUnit, sizeof(card.centerUnit),
                    ru ? "КМ | %.1f Л/100" : "KM | %.1f L/100",
                    latestDay_.averageL100);
    } else {
      std::snprintf(card.centerUnit, sizeof(card.centerUnit), "%s",
                    ru ? "КМ | ПРОГРЕВ" : "KM | WARM-UP");
    }

    std::snprintf(card.leftValue, sizeof(card.leftValue),
                  ru ? "%.2f л" : "%.2f L", latestDay_.petrolLiters);
    std::snprintf(card.rightValue, sizeof(card.rightValue),
                  ru ? "%.2f л" : "%.2f L", latestDay_.lpgLiters);
    std::snprintf(card.footer, sizeof(card.footer), "%s",
                  ru ? "СУТКИ" : "DAY");
    return card;
  }

  const size_t tripIndex =
      hasLatestDay_ ? (effectiveCursor - 1U) : effectiveCursor;
  const TripRecord& rec = recent_[tripIndex];
  const TripComputedMetrics m = TripRecordCodec::computeMetrics(rec);

  char dur[14];
  const uint32_t durSec =
      m.wallDurationSec > 0U ? m.wallDurationSec : m.engineSeconds;
  formatCompactDuration(durSec, ru, dur, sizeof(dur));

  if (m.dateUnknown) {
    std::snprintf(card.subtitle, sizeof(card.subtitle),
                  ru ? "#%lu | БЕЗ ДАТЫ | %s" : "#%lu | NO DATE | %s",
                  static_cast<unsigned long>(rec.sequence), dur);
  } else {
    char ddmm[6];
    extractDayMonth(m.localDate, ddmm);
    std::snprintf(card.subtitle, sizeof(card.subtitle), "#%lu | %s | %s",
                  static_cast<unsigned long>(rec.sequence), ddmm, dur);
  }

  std::snprintf(card.centerValue, sizeof(card.centerValue), "%.1f",
                m.distanceKm);
  if (m.hasAverageL100) {
    std::snprintf(card.centerUnit, sizeof(card.centerUnit),
                  ru ? "КМ | %.1f Л/100" : "KM | %.1f L/100", m.averageL100);
  } else {
    std::snprintf(card.centerUnit, sizeof(card.centerUnit), "%s",
                  ru ? "КМ | ПРОГРЕВ" : "KM | WARM-UP");
  }

  std::snprintf(card.leftValue, sizeof(card.leftValue),
                ru ? "%.2f л" : "%.2f L", m.petrolLiters);
  std::snprintf(card.rightValue, sizeof(card.rightValue),
                ru ? "%.2f л" : "%.2f L", m.lpgLiters);
  std::snprintf(card.footer, sizeof(card.footer), ru ? "%u ИЗ %u" : "%u OF %u",
                static_cast<unsigned>(tripIndex + 1U),
                static_cast<unsigned>(recentCount_));
  return card;
}
