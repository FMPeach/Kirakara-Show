#include "../apps/show_host/overlay/announcement_scroll.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <array>

void expect(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
int main() {
    for (const double length : {10., 300., 2000., 12000.}) {
        AnnouncementScroll scroll;
        auto first=scroll.sample(1,1280,length,100);
        expect(first.count==1 && first.x(0)==1280,"intro must have no preceding tail");
        const double period=(length+96)/62;
        std::array times{.016,1.,10.,period-.00001,period,period+.00001,3600.,86400.*20};
        std::sort(times.begin(),times.end());
        for (double t : times) {
            auto copies=scroll.sample(1,1280,length,100+t);
            expect(copies.count<=static_cast<std::size_t>(std::ceil((1280+length)/(length+96)))+1,
                "copy count must not grow with elapsed time");
            const auto direct=1280-62*t;
            for (std::size_t i=0;i<copies.count;++i) {
                auto n=std::round((copies.x(i)-direct)/(length+96));
                expect(n>=0 && std::abs((direct+n*(length+96))-copies.x(i))<.00001,
                    "loop positions must match the continuous sequence");
                if (i) expect(std::abs(copies.x(i-1)-copies.x(i)-length-96)<.00001,"fixed gap");
            }
        }
        expect(scroll.sample(2,1280,length,2000000).x(0)==1280,"same text resubmission restarts");
        expect(scroll.sample(2,1280,length,2000001).x(0)==1218,"same publication keeps moving");
        scroll.hide();
        expect(scroll.sample(3,1280,length,2000010).x(0)==1280,"reveal uses latest submission");
        expect(scroll.sample(3,1820,length,2000011).x(0)==1820,"width change restarts");
    }
    AnnouncementScroll scroll;
    expect(!scroll.sample(1,0,100,1).count,"zero width");
    expect(!scroll.sample(1,100,0,1).count,"empty text");
    expect(!scroll.sample(1,100,10,std::numeric_limits<double>::quiet_NaN()).count,"invalid time");
    std::cout << "Announcement continuous-time scroll tests passed.\n";
}
