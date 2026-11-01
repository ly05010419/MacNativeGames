#nullable enable
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace Ryujinx.Common.Memory
{
    /// <summary>
    /// An <see cref="IMemoryOwner{T}"/> implementation with an embedded length and fast <see cref="Span{T}"/>
    /// accessor, with memory allocated from <see cref="ArrayPooling"/>.
    /// </summary>
    /// <typeparam name="T">The type of item to store.</typeparam>
    public sealed class MemoryOwner<T> : IMemoryOwner<T>
    {
        private static class ArrayPooling
        {
            public class Holder(T[]? array = null) : IComparable<Holder>, IComparable<int>
            {
                public int SkipCount;
                public readonly T[]? Array = array;

                public int CompareTo(Holder? other)
                {
                    return Array!.Length.CompareTo(other!.Array!.Length);
                }
                
                public int CompareTo(int other)
                {
                    int self = Array!.Length;

                    if (self < other)
                    {
                        SkipCount++;
                        return -1;
                    }

                    if (self > other * 4)
                    {
                        return 1;
                    }
                    
                    return 0;
                }
            }
            
            // ReSharper disable once StaticMemberInGenericType
            private static int _maxCacheCount = 50;
            
            private const int MaxSkipCount = 50;
        
            static readonly List<Holder> _pool = new();
            
            // ReSharper disable once StaticMemberInGenericType
            static readonly Lock _lock = new();
            
            private static int BinarySearch(List<Holder> list, int size)
            {
                int min = 0;
                int max = list.Count-1;

                while (min <= max)
                {
                    int mid = (min + max) / 2;
                    int comparison = list[mid].CompareTo(size);
                    if (comparison == 0)
                    {
                        return mid;
                    }
                    if (comparison < 0)
                    {
                        min = mid+1;
                    }
                    else
                    {
                        max = mid-1;
                    }
                }
                return ~min;
            }

            public static T[] Get(int minimumSize)
            {
                lock (_lock)
                {
                    int index = BinarySearch(_pool, minimumSize);

                    if (index >= 0)
                    {
                        Holder holder = _pool[index];
                        
                        _pool.Remove(holder);
                        return holder.Array!;
                    }

                    return new T[minimumSize];
                }
            }

            public static void Return(T[] array)
            {

                lock (_lock)
                {
                    Holder holder = new(array);
                    int i = _pool.BinarySearch(holder);
                    if (i < 0)
                    {
                        _pool.Insert(~i, holder);
                    }

                    if (_pool.Count >= _maxCacheCount)
                    {
                        for (int index = 0; index < _pool.Count; index++)
                        {
                            Holder h = _pool[index];

                            if (h.SkipCount >= MaxSkipCount)
                            {
                                _pool.Remove(h);
                                index--;
                            }
                        }
                        
                        _maxCacheCount = _pool.Count * 2;
                    }
                }
            }
        }
        
        private readonly int _length;
        private T[]? _array;

        /// <summary>
        /// Initializes a new instance of the <see cref="MemoryOwner{T}"/> class with the specified parameters.
        /// </summary>
        /// <param name="length">The length of the new memory buffer to use</param>
        private MemoryOwner(int length)
        {
            _length = length;
            _array = ArrayPooling.Get(length);
        }

        /// <summary>
        /// Creates a new <see cref="MemoryOwner{T}"/> instance with the specified length.
        /// </summary>
        /// <param name="length">The length of the new memory buffer to use</param>
        /// <returns>A <see cref="MemoryOwner{T}"/> instance of the requested length</returns>
        /// <exception cref="ArgumentOutOfRangeException">Thrown when <paramref name="length"/> is not valid</exception>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static MemoryOwner<T> Rent(int length) => new(length);

        /// <summary>
        /// Creates a new <see cref="MemoryOwner{T}"/> instance with the specified length and the content cleared.
        /// </summary>
        /// <param name="length">The length of the new memory buffer to use</param>
        /// <returns>A <see cref="MemoryOwner{T}"/> instance of the requested length and the content cleared</returns>
        /// <exception cref="ArgumentOutOfRangeException">Thrown when <paramref name="length"/> is not valid</exception>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static MemoryOwner<T> RentCleared(int length)
        {
            MemoryOwner<T> result = new(length);

            result._array.AsSpan(0, length).Clear();

            return result;
        }

        /// <summary>
        /// Creates a new <see cref="MemoryOwner{T}"/> instance with the content copied from the specified buffer.
        /// </summary>
        /// <param name="buffer">The buffer to copy</param>
        /// <returns>A <see cref="MemoryOwner{T}"/> instance with the same length and content as <paramref name="buffer"/></returns>
        public static MemoryOwner<T> RentCopy(ReadOnlySpan<T> buffer)
        {
            MemoryOwner<T> result = new(buffer.Length);

            buffer.CopyTo(result._array);

            return result;
        }

        /// <summary>
        /// Gets the number of items in the current instance.
        /// </summary>
        public int Length
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get => _length;
        }

        /// <inheritdoc/>
        public Memory<T> Memory
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get
            {
                T[]? array = _array;

                if (array is null)
                {
                    ThrowObjectDisposedException();
                }

                return new(array, 0, _length);
            }
        }

        /// <summary>
        /// Gets a <see cref="Span{T}"/> wrapping the memory belonging to the current instance.
        /// </summary>
        /// <remarks>
        /// Uses a trick made possible by the .NET 6+ runtime array layout.
        /// </remarks>
        public Span<T> Span
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get
            {
                T[]? array = _array;

                if (array is null)
                {
                    ThrowObjectDisposedException();
                }

                ref T firstElementRef = ref MemoryMarshal.GetArrayDataReference(array);

                return MemoryMarshal.CreateSpan(ref firstElementRef, _length);
            }
        }

        /// <inheritdoc/>
        public void Dispose()
        {
            T[]? array = Interlocked.Exchange(ref _array, null);

            if (array is not null)
            {
                ArrayPooling.Return(array);
            }
        }

        /// <summary>
        /// Throws an <see cref="ObjectDisposedException"/> when <see cref="_array"/> is <see langword="null"/>.
        /// </summary>
        [DoesNotReturn]
        private static void ThrowObjectDisposedException()
        {
            throw new ObjectDisposedException(nameof(MemoryOwner<T>), "The buffer has already been disposed.");
        }
    }
}
