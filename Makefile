CXX      := g++
CXXFLAGS := -std=c++26 -Wall -Wextra -Oz -flto -ffunction-sections -fdata-sections -pipe \
            -fno-semantic-interposition -fomit-frame-pointer \
            -fno-stack-protector -fno-math-errno -fno-ident
LDFLAGS  := -lcurl -lpthread -Wl,-z,now -Wl,--gc-sections -Wl,-s \
            -Wl,--build-id=none -Wl,--icf=all
TARGET   := zen_proxy

.PHONY: all clean run

all: $(TARGET)
	llvm-objcopy --remove-section=.comment \
	             --remove-section=.note.android.ident \
	             --remove-section=.gnu.version \
	             --remove-section=.eh_frame \
	             --remove-section=.eh_frame_hdr \
	             --remove-section=.gcc_except_table \
	             $(TARGET)

$(TARGET): zen_proxy.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET) $(ARGS)
