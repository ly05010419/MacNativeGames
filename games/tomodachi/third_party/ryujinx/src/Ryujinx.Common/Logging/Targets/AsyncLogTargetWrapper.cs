using System;
using System.Collections.Concurrent;
using System.Threading;

namespace Ryujinx.Common.Logging.Targets
{
    public enum AsyncLogTargetOverflowAction
    {
        /// <summary>
        /// Block until there's more room in the queue
        /// </summary>
        Block = 0,

        /// <summary>
        /// Discard the overflowing item
        /// </summary>
        Discard = 1,
    }

    public class AsyncLogTargetWrapper : ILogTarget
    {
        private readonly ILogTarget _target;

        private readonly Thread _messageThread;

        private readonly BlockingCollection<LogEventArgs> _messageQueue;

        private readonly int _overflowTimeout;

        private sealed class FlushEventArgs : LogEventArgs
        {
            public readonly ManualResetEventSlim SignalEvent;

            public FlushEventArgs(ManualResetEventSlim signalEvent)
                : base(LogLevel.Notice, TimeSpan.Zero, string.Empty, string.Empty)
            {
                SignalEvent = signalEvent;
            }
        }

        string ILogTarget.Name => _target.Name;

        public AsyncLogTargetWrapper(ILogTarget target, int queueLimit = -1, AsyncLogTargetOverflowAction overflowAction = AsyncLogTargetOverflowAction.Block)
        {
            _target = target;
            _messageQueue = new BlockingCollection<LogEventArgs>(queueLimit);
            _overflowTimeout = overflowAction == AsyncLogTargetOverflowAction.Block ? -1 : 0;

            _messageThread = new Thread(() =>
            {
                while (!_messageQueue.IsCompleted)
                {
                    try
                    {
                        LogEventArgs item = _messageQueue.Take();

                        if (item is FlushEventArgs flush)
                        {
                            flush.SignalEvent.Set();
                            continue;
                        }

                        _target.Log(this, item);
                    }
                    catch (InvalidOperationException)
                    {
                        // IOE means that Take() was called on a completed collection.
                        // Some other thread can call CompleteAdding after we pass the
                        // IsCompleted check but before we call Take.
                        // We can simply catch the exception since the loop will break
                        // on the next iteration.
                    }
                }
            })
            {
                Name = "Logger.MessageThread",
                IsBackground = true,
            };
            _messageThread.Start();
        }

        public void Log(object sender, LogEventArgs e)
        {
            if (!_messageQueue.IsAddingCompleted)
            {
                _messageQueue.TryAdd(e, _overflowTimeout);
            }
        }

        public void Flush()
        {
            if (_messageQueue.Count == 0 || _messageQueue.IsAddingCompleted)
            {
                return;
            }

            using ManualResetEventSlim signal = new ManualResetEventSlim(false);
            try
            {
                _messageQueue.Add(new FlushEventArgs(signal));
            }
            catch (InvalidOperationException)
            {
                return;
            }

            signal.Wait();
        }

        public void Dispose()
        {
            GC.SuppressFinalize(this);
            _messageQueue.CompleteAdding();
            _messageThread.Join();
        }
    }
}
