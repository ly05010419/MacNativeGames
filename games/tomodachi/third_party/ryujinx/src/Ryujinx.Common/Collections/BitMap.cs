namespace Ryujinx.Common.Collections
{
    /// <summary>
    /// Represents a collection that can store 1 bit values.
    /// </summary>
    public struct BitMap
    {
        /// <summary>
        /// Size in bits of the integer used internally for the groups of bits.
        /// </summary>
        public const int IntSize = 64;

        private const int IntShift = 6;
        private const int IntMask = IntSize - 1;

        private readonly long[] _masks;

        /// <summary>
        /// Gets or sets the value of a bit.
        /// </summary>
        /// <param name="bit">Bit to access</param>
        /// <returns>Bit value</returns>
        public bool this[int bit]
        {
            get => IsSet(bit);
            set
            {
                if (value)
                {
                    Set(bit);
                }
                else
                {
                    Clear(bit);
                }
            }
        }

        /// <summary>
        /// Creates a new bitmap.
        /// </summary>
        /// <param name="count">Total number of bits</param>
        public BitMap(int count)
        {
            _masks = new long[(count + IntMask) / IntSize];
        }

        /// <summary>
        /// Checks if any bit is set.
        /// </summary>
        /// <returns>True if any bit is set, false otherwise</returns>
        public bool AnySet()
        {
            for (int i = 0; i < _masks.Length; i++)
            {
                if (_masks[i] != 0)
                {
                    return true;
                }
            }

            return false;
        }

        /// <summary>
        /// Checks if a specific bit is set.
        /// </summary>
        /// <param name="bit">Bit to be checked</param>
        /// <returns>True if set, false otherwise</returns>
        public bool IsSet(int bit)
        {
            int wordIndex = bit >> IntShift;
            int wordBit = bit & IntMask;

            long wordMask = 1L << wordBit;

            return (_masks[wordIndex] & wordMask) != 0;
        }

        /// <summary>
        /// Checks if any bit inside a given range of bits is set.
        /// </summary>
        /// <param name="start">Start bit of the range</param>
        /// <param name="end">End bit of the range (inclusive)</param>
        /// <returns>True if any bit is set, false otherwise</returns>
        public bool IsSet(int start, int end)
        {
            if (start == end)
            {
                return IsSet(start);
            }

            int startIndex = start >> IntShift;
            int startBit = start & IntMask;
            long startMask = -1L << startBit;

            int endIndex = end >> IntShift;
            int endBit = end & IntMask;
            long endMask = (long)(ulong.MaxValue >> (IntMask - endBit));

            if (startIndex == endIndex)
            {
                return (_masks[startIndex] & startMask & endMask) != 0;
            }

            if ((_masks[startIndex] & startMask) != 0)
            {
                return true;
            }

            for (int i = startIndex + 1; i < endIndex; i++)
            {
                if (_masks[i] != 0)
                {
                    return true;
                }
            }

            if ((_masks[endIndex] & endMask) != 0)
            {
                return true;
            }

            return false;
        }

        /// <summary>
        /// Sets the value of a bit to 1.
        /// </summary>
        /// <param name="bit">Bit to be set</param>
        /// <returns>True if the bit was 0 and then changed to 1, false if it was already 1</returns>
        public bool Set(int bit)
        {
            int wordIndex = bit >> IntShift;
            int wordBit = bit & IntMask;

            long wordMask = 1L << wordBit;

            if ((_masks[wordIndex] & wordMask) != 0)
            {
                return false;
            }

            _masks[wordIndex] |= wordMask;

            return true;
        }

        /// <summary>
        /// Sets a given range of bits to 1.
        /// </summary>
        /// <param name="start">Start bit of the range</param>
        /// <param name="end">End bit of the range (inclusive)</param>
        public void SetRange(int start, int end)
        {
            if (start == end)
            {
                Set(start);
                return;
            }

            int startIndex = start >> IntShift;
            int startBit = start & IntMask;
            long startMask = -1L << startBit;

            int endIndex = end >> IntShift;
            int endBit = end & IntMask;
            long endMask = (long)(ulong.MaxValue >> (IntMask - endBit));

            if (startIndex == endIndex)
            {
                _masks[startIndex] |= startMask & endMask;
            }
            else
            {
                _masks[startIndex] |= startMask;

                for (int i = startIndex + 1; i < endIndex; i++)
                {
                    _masks[i] |= -1;
                }

                _masks[endIndex] |= endMask;
            }
        }

        /// <summary>
        /// Sets a given bit to 0.
        /// </summary>
        /// <param name="bit">Bit to be cleared</param>
        public void Clear(int bit)
        {
            int wordIndex = bit >> IntShift;
            int wordBit   = bit & IntMask;

            long wordMask = 1L << wordBit;

            _masks[wordIndex] &= ~wordMask;
        }

        /// <summary>
        /// Sets all bits to 0.
        /// </summary>
        public void Clear()
        {
            for (int i = 0; i < _masks.Length; i++)
            {
                _masks[i] = 0;
            }
        }

        /// <summary>
        /// Sets one or more groups of bits to 0.
        /// See <see cref="IntSize"/> for how many bits are inside each group.
        /// </summary>
        /// <param name="start">Start index of the group</param>
        /// <param name="end">End index of the group (inclusive)</param>
        public void ClearInt(int start, int end)
        {
            for (int i = start; i <= end; i++)
            {
                _masks[i] = 0;
            }
        }
    }
}