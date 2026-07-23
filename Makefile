CXX      := g++
TARGET   := zen_proxy

# ── Android / Termux ─────────────────────────────────────────────────────
CXXFLAGS_A = -std=c++26 -Wall -Wextra -Oz -flto -ffunction-sections \
             -fdata-sections -pipe -fno-semantic-interposition \
             -fomit-frame-pointer -fno-stack-protector \
             -fno-math-errno -fno-ident
LDFLAGS_A  = -lcurl -lpthread -Wl,-z,now -Wl,--gc-sections -Wl,-s \
             -Wl,--build-id=none -Wl,--icf=all

# ── Linux (x86_64) ──────────────────────────────────────────────────────
CXXFLAGS_L = -std=c++26 -Wall -Wextra -Os -flto -ffunction-sections \
             -fdata-sections -pipe -fno-semantic-interposition \
             -fomit-frame-pointer -fno-stack-protector -fno-math-errno
LDFLAGS_L  = -lcurl -lpthread -Wl,-z,now -Wl,--gc-sections -Wl,-s \
             -Wl,--build-id=none -Wl,--icf=all

# ── Windows (MinGW-w64) ────────────────────────────────────────────────
CXXFLAGS_W = -std=c++26 -Wall -Wextra -Os -flto -ffunction-sections \
             -fdata-sections -pipe -fomit-frame-pointer \
             -fno-stack-protector -fno-math-errno
LDFLAGS_W  = -lcurl -lpthread -Wl,--gc-sections -Wl,-s -Wl,--build-id=none

.PHONY: all android linux windows clean

all android:
	$(CXX) $(CXXFLAGS_A) -o $(TARGET) zen_proxy.cpp $(LDFLAGS_A) 2>&1
	llvm-objcopy --remove-section=.comment \
	  --remove-section=.note.android.ident \
	  --remove-section=.gnu.version \
	  --remove-section=.eh_frame \
	  --remove-section=.eh_frame_hdr \
	  --remove-section=.gcc_except_table \
	  $(TARGET) 2>/dev/null || true

linux:
	g++ -std=c++26 $(CXXFLAGS_L) -o $(TARGET) zen_proxy.cpp $(LDFLAGS_L)
	strip --remove-section=.comment --remove-section=.gnu.version \
	  --remove-section=.eh_frame --remove-section=.eh_frame_hdr \
	  --remove-section=.gcc_except_table $(TARGET) 2>/dev/null || true

windows:
	$(CXX) $(CXXFLAGS_W) -o $(TARGET).exe zen_proxy.cpp $(LDFLAGS_W)

clean:
	rm -f $(TARGET) $(TARGET).exe
