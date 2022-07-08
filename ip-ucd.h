#include <stdexcept>
#include <vwpp-3.0.h>

// Open the IPCUD namespace for forward definitions.

namespace IPUCD {
    namespace v1_0 {

	// Defines an entry in the FIFO. The FIFO contents are a
	// 32-bit integer where the top 24 bits are the microsecond
	// count since the last reset event (typically $02.) The
	// lowest 8 bits represent the TCLK event.

	class FifoEntry {
	    static uint32_t const NO_VALUE = 0xffffffff;

	    uint32_t const value;

	 public:
	    explicit FifoEntry(uint32_t v = NO_VALUE) : value(v) {}

	    uint8_t event() const { return uint8_t(value); }
	    uint32_t stamp() const { return value >> 8; }
	    bool isValid() const { return value != NO_VALUE; }
	};
    }
}

// Have to re-open the vwpp::v3_0 namespace to insert a new
// template. Some C++ template ugliness.

namespace vwpp {
    namespace v3_0 {
	namespace VME {
	    using namespace IPUCD::v1_0;

	    // This template allows us to define a virtual register
	    // that returns `FifoEntry` objects. The hardware actually
	    // requires two, 16-bit reads to obtain the entry. The
	    // registers are assumed to be adjacent and the offset is
	    // assumed to be of the lower addressed register.

	    template <size_t Offset, ReadAccess R>
	    struct Register<A32, FifoEntry, Offset, R, NoWrite> {
		typedef FifoEntry Type;
		typedef uint16_t AtomicType;

		static AddressSpace const space = A32;

		enum { RegOffset = Offset, RegEntries = 1 };

		static Type read(uint8_t volatile* const base)
		{
		    typedef ReadAPI<AtomicType, Offset, R> API;

		    uint32_t tmp = API::readMem(base, 0);

		    return FifoEntry((tmp << 16) | API::readMem(base, 1));
		}
	    };
	}
    }
}

namespace IPUCD {
    namespace v1_0 {
	using namespace vwpp::v3_0;

	// A helper template to define "registers" that reside in the
	// Industry Pack's PROM space (like module IDs.) To define a
	// location, you only need to specify an offset.

	template <size_t Offset>
	struct PROM
	    : public VME::Register<VME::A16, uint8_t, Offset, VME::Read, VME::NoWrite>
	{};

	// Define control register commands.

	enum ControlCommand {
	    NoOp = 0x0,
	    EnableTCLK = 0x1,
	    DisableTCLK = 0x2,
	    EnableMDAT = 0x3,
	    DisableMDAT = 0x4,
	    MDAT_Buf0 = 0x5,
	    MDAT_Buf1 = 0x6,
	    EnableMDAT_BufAuto = 0x7,
	    DisableMDAT_BufAuto = 0x8,
	    SW_Interrupt = 0x9,
	    SW_Reset = 0xff
	};

	// Provides an API to control and interface an IP-UCD industry
	// pack. An instance of this class is self-contained in that
	// it will provide serialization primitives so that it can
	// correctly be used with interrupts and multiple threads
	// (tasks.)

	class HW {

	    // Define our serialization primitive and a simplified
	    // type definition (`LockType`) for describing it.

	    Mutex mutex;

	 public:
	    typedef Mutex::PMLockWithInt<HW, &HW::mutex> LockType;

	 private:

	    // Define two address spaces used by the module. The A16
	    // space contains registers to control and monitor the
	    // state of the hardware. The A32 memory holds the
	    // incoming TCLK events with their timestamps.

	    typedef VME::Memory<VME::A16, VME::D8_D16, 0x100, LockType> A16;
	    typedef VME::Memory<VME::A32, VME::D16, 0x2000, LockType> A32;

	    // Define the registers in A16 space.

	    typedef VME::Register<VME::A16, uint16_t, 0x40, VME::Read, VME::ConfirmWrite> regControl;
	    typedef VME::Register<VME::A16, uint16_t, 0x42, VME::Read, VME::ConfirmWrite> regStatus;
	    typedef VME::Register<VME::A16, uint8_t, 0x44, VME::Read, VME::ConfirmWrite> regMdatIntType;
	    typedef VME::Register<VME::A16, uint8_t, 0x45, VME::Read, VME::ConfirmWrite> regMdatBufSwitch;
	    typedef VME::Register<VME::A16, uint16_t, 0x46, VME::Read, VME::ConfirmWrite> regFtpTSLow;
	    typedef VME::Register<VME::A16, uint16_t, 0x48, VME::Read, VME::ConfirmWrite> regFtpTSHigh;
	    typedef VME::Register<VME::A16, uint8_t, 0x4a, VME::Read, VME::ConfirmWrite> regFifoWrite;
	    typedef VME::Register<VME::A16, uint8_t, 0x4b, VME::Read, VME::ConfirmWrite> regFifoClear;
	    typedef VME::Register<VME::A16, uint16_t, 0x4c, VME::Read, VME::ConfirmWrite> regFifoThreshold;

	    // Define registers in A32 space.

	    typedef VME::Register<VME::A32, uint16_t[256], 0x0, VME::Read, VME::ConfirmWrite> regTrigger;
	    typedef VME::Register<VME::A32, FifoEntry, 0x1200, VME::DestructiveRead, VME::NoWrite> regFifo;

	    // Define the actual memory space objects which will
	    // control access to the hardware.

	    A16 const a16;
	    A32 const a32;

	    // --- Define some private, helper methods. ---

	    // Define status bits.

	    enum Status {
		MDatParityError =	0x4000,
		MDatBuffer0_1 =		0x2000,
		FIFOUnderflow =		0x1000,
		FIFOOverflow =		0x0800,
		FIFOFull =		0x0400,
		FIFOThreshold =		0x0200,
		FIFOEmpty =		0x0100,
		TclkParityError =	0x0080,
		MdatBuffer1Enabled =	0x0040,
		MdatBuffer0Enabled =	0x0020,
		MdatAutoBufferEnabled =	0x0010,
		MDatEnabled =		0x0008,
		TclkEnabled =		0x0004,
		MDatPresent =		0x0002,
		TclkPresent =		0x0001
	    };

	    // Returns the module ID located in the standard PROM
	    // area. This method defines it own, local typedefs since
	    // the module ID locations shouldn't be used by any other
	    // function. Since they are from unchanging, ROM contents,
	    // we don't worry about which way the compiler decides to
	    // calculate the return expression.

	    uint16_t getModuleId(LockType const& lock)
	    {
		typedef PROM<0x89> regIdHigh;
		typedef PROM<0x8b> regIdLow;

		return ((uint16_t) a16.get<regIdHigh>(lock) << 8) +
		    (uint16_t) a16.get<regIdLow>(lock);
	    }

	    // Sets the FIFO threshold value. Even though the register
	    // is 16 bits wide, it can only accept a subset of values.
	    // If the caller provides a bad value, it's a programming
	    // error.

	    void setFifoThreshold(LockType const& lock, uint8_t const level)
	    {
		if (level > 0)
		    a16.set<regFifoThreshold>(lock, level);
		else
		    throw std::logic_error("illegal FIFO threshold value");
	    }

	    void setupInterrupt(IntLock const&)
	    {
	    }

	    // Associates an incoming with a trigger. The parameter
	    // `enable` enabled or disables the trigger level. `event`
	    // is the event (0 to 255). `trigBit` is the trigger bit
	    // (1 - 15).

	    void adjustTclkReception(LockType const& lock, bool const enable,
				     uint8_t const event,
				     uint8_t const trigBit)
	    {
		if (trigBit > 7)
		    throw std::logic_error("illegal trigger bit value");

		setupInterrupt(lock);

		uint8_t const mask = 1 << trigBit;
		uint16_t const prev = a32.get_element<regTrigger>(lock, event);
		uint16_t const value = enable ? (prev | mask) : (prev & ~mask);

		a32.set_element<regTrigger>(lock, event, value);
	    }

	    // Returns `true` or `false` based whether the specified
	    // event activates the specified trigger.

	    bool getTclkReception(LockType const& lock,
				  uint8_t const event,
				  uint8_t const trigBit)
	    {
		if (trigBit > 7)
		    throw std::logic_error("illegal trigger bit value");

		uint8_t const mask = 1 << trigBit;

		return (a32.get_element<regTrigger>(lock, event) & mask) != 0;
	    }

	    // Sets the trigger which resets the timestamp used to tag
	    // events in the FIFO.

	    void setResetFifoTimestampTrigger(LockType const& lock,
					      uint8_t const trigBit)
	    {
		if (trigBit < 1 || trigBit > 7)
		    throw std::logic_error("illegal trigger bit value");

		a16.set<regFifoClear>(lock, trigBit + 1);
	    }

	    // Sets the trigger which writes to the FIFO.

	    void setWriteFifoTrigger(LockType const& lock,
				     uint8_t const trigBit)
	    {
		if (trigBit < 1 || trigBit > 7)
		    throw std::logic_error("illegal trigger bit value");

		a16.set<regFifoWrite>(lock, trigBit + 1);
	    }

	    Status getStatus(LockType const& lock)
	    {
		uint16_t const temp = a16.get<regStatus>(lock);

		a16.set<regStatus>(lock, temp);
		return Status(temp);
	    }

	    // Returns the oldest entry in the FIFO. If the FIFO is
	    // empty, it returns an invalid value which can be tested
	    // using the `.isValid()` method.

	 public:
	    FifoEntry readFifo(LockType const& lock)
	    {
		if (UNLIKELY((a16.get<regStatus>(lock) & FIFOEmpty) == 0))
		    return a32.get<regFifo>(lock);
		else
		    return FifoEntry();
	    }

	public:
	    // Creates an instance of the driver and initializes the
	    // associated hardware. If this constructor completes
	    // successfully, the IP-UCD is ready to go. If it throws
	    // an exception, the state of the hardware will be
	    // untouched.

	    HW(size_t const a16_offset, size_t const a32_offset)
		: a16(a16_offset), a32(a32_offset)
	    {
		LockType const lock(this);

		// Look for the IP-UCD module ID.

		if (getModuleId(lock) != 0xbb15)
		    throw std::runtime_error("IP-UCD not found at A16 offset");

		// Perform a software reset.

		a16.set<regControl>(lock, SW_Reset);

		// Clear trigger memory.

		a16.set<regFifoWrite>(lock, 0x00);
		a16.set<regFifoClear>(lock, 0x00);

		for (size_t ii = 0; ii < 256; ++ii)
		    a32.set_element<regTrigger>(lock, ii, 0x00);

		// Start collecting TCLK events.

		a16.set<regControl>(lock, EnableTCLK);
	    }
	};

    }
}

// Local variables:
// mode: c++
// End:
