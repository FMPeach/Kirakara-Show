#include "../apps/show_host/overlay/announcement_scroll.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

void expect(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main() {
    constexpr auto speed = AnnouncementScroll::speed;
    for (const double width : {100., 1280., 1820.}) {
        for (const double length : {1., 10., 300., 2000., 12000.}) {
            AnnouncementScroll scroll;
            const auto first = scroll.sample(1, width, length, 100);
            expect(first.valid && first.x == width, "intro starts outside the right edge");
            const double period = (width + length) / speed;
            std::array times{.016, 1., 10., period - .00001,
                period, period + .00001, 3600., 86400. * 20};
            std::sort(times.begin(), times.end());
            for (const double t : times) {
                const auto position = scroll.sample(1, width, length, 100 + t);
                expect(position.valid, "single line remains valid over long runs");
                expect(position.x <= width && position.x >= -length,
                    "only one line traverses the viewport");
                const auto expected = width - std::fmod(speed * t, width + length);
                // At a rounded boundary both possible positions have all ink
                // outside the viewport, either at the exit or at the entrance.
                const auto delta = std::abs(position.x - expected);
                expect(delta < .00001 || std::abs(delta - width - length) < .00001,
                    "motion follows elapsed time, not frame count");
            }

            AnnouncementScroll boundary;
            const auto initial = boundary.sample(1, width, length, 0);
            expect(initial.valid, "boundary fixture starts");
            const auto before = boundary.sample(1, width, length, period - .001);
            const auto at = boundary.sample(1, width, length, period);
            const auto after = boundary.sample(1, width, length, period + .001);
            expect(std::abs(before.x + length - speed * .001) < .00001,
                "previous line must finish leaving before wrapping");
            expect(std::abs(at.x - width) < .00001 || std::abs(at.x + length) < .00001,
                "cycle boundary contains no visible tail or new head");
            expect(std::abs(after.x - width + speed * .001) < .00001,
                "next line enters from the right only after the old tail exits");

            expect(scroll.sample(2, width, length, 2000000).x == width,
                "same text resubmission restarts");
            expect(scroll.sample(2, width, length, 2000001).x == width - speed,
                "same publication keeps moving");
            scroll.hide();
            expect(scroll.sample(2, width, length, 2000010).x == width,
                "reveal restarts the same publication");
            expect(scroll.sample(2, width + 100, length, 2000011).x == width + 100,
                "width change restarts");
            expect(scroll.sample(2, width + 100, length, 1).x == width + 100,
                "monotonic-source reset restarts safely");
        }
    }
    AnnouncementScroll scroll;
    expect(!scroll.sample(1, 0, 100, 1).valid, "zero width");
    expect(!scroll.sample(1, 100, 0, 1).valid, "empty text");
    expect(!scroll.sample(1, 100, 10, std::numeric_limits<double>::quiet_NaN()).valid,
        "invalid time");
    std::cout << "Announcement single-copy continuous-time scroll tests passed.\n";
}
