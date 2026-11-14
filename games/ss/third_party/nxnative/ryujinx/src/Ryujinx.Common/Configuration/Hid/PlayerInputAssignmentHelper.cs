using Ryujinx.Common.Configuration.Hid.Keyboard;
using System;
using System.Collections.Generic;
using System.Linq;

namespace Ryujinx.Common.Configuration.Hid
{
    public static class PlayerInputAssignmentHelper
    {
        public static AssignedInputDevice CreatePrimaryDevice(InputConfig inputConfig)
        {
            if (inputConfig == null || string.IsNullOrWhiteSpace(inputConfig.Id))
            {
                return null;
            }

            return new AssignedInputDevice
            {
                Type = inputConfig is StandardKeyboardInputConfig
                    ? AssignedInputDeviceType.Keyboard
                    : AssignedInputDeviceType.Controller,
                Id = inputConfig.Id,
            };
        }

        public static PlayerInputAssignment Normalize(PlayerInputAssignment assignment, AssignedInputDevice preferredDevice = null)
        {
            if (assignment == null)
            {
                return null;
            }

            PlayerInputAssignment normalized = new()
            {
                PlayerIndex = assignment.PlayerIndex,
                EnableDynamicInputSwap = assignment.EnableDynamicInputSwap,
            };

            List<AssignedInputDevice> distinctDevices = Deduplicate(assignment.Devices);

            if (assignment.EnableDynamicInputSwap)
            {
                normalized.Devices.AddRange(distinctDevices.Select(Clone));
                return normalized;
            }

            AssignedInputDevice primaryDevice = SelectPrimaryDevice(distinctDevices, preferredDevice) ?? Clone(preferredDevice);

            if (primaryDevice != null)
            {
                normalized.Devices.Add(Clone(primaryDevice));
            }

            return normalized;
        }

        public static bool AreEquivalent(
            PlayerInputAssignment left,
            PlayerInputAssignment right,
            AssignedInputDevice leftPreferredDevice = null,
            AssignedInputDevice rightPreferredDevice = null)
        {
            if (left == null || right == null)
            {
                return left == right;
            }

            PlayerInputAssignment normalizedLeft = Normalize(left, leftPreferredDevice);
            PlayerInputAssignment normalizedRight = Normalize(right, rightPreferredDevice);

            if (normalizedLeft.EnableDynamicInputSwap != normalizedRight.EnableDynamicInputSwap)
            {
                return false;
            }

            List<(AssignedInputDeviceType Type, string Id, string ProfileName)> leftDevices = normalizedLeft.Devices
                .Select(device => (Type: device.Type, Id: device.Id, ProfileName: device.ProfileName ?? string.Empty))
                .OrderBy(device => device.Type)
                .ThenBy(device => device.Id, StringComparer.Ordinal)
                .ThenBy(device => device.ProfileName, StringComparer.Ordinal)
                .ToList();

            List<(AssignedInputDeviceType Type, string Id, string ProfileName)> rightDevices = normalizedRight.Devices
                .Select(device => (Type: device.Type, Id: device.Id, ProfileName: device.ProfileName ?? string.Empty))
                .OrderBy(device => device.Type)
                .ThenBy(device => device.Id, StringComparer.Ordinal)
                .ThenBy(device => device.ProfileName, StringComparer.Ordinal)
                .ToList();

            return leftDevices.SequenceEqual(rightDevices);
        }

        private static List<AssignedInputDevice> Deduplicate(IEnumerable<AssignedInputDevice> devices)
        {
            List<AssignedInputDevice> result = [];

            if (devices == null)
            {
                return result;
            }

            foreach (AssignedInputDevice device in devices)
            {
                if (device == null || string.IsNullOrWhiteSpace(device.Id))
                {
                    continue;
                }

                int existingIndex = result.FindIndex(existing =>
                    existing.Type == device.Type &&
                    string.Equals(existing.Id, device.Id, StringComparison.Ordinal));

                if (existingIndex == -1)
                {
                    result.Add(Clone(device));
                    continue;
                }

                if (!string.IsNullOrWhiteSpace(device.ProfileName) ||
                    string.IsNullOrWhiteSpace(result[existingIndex].ProfileName))
                {
                    result[existingIndex].ProfileName = device.ProfileName;
                }
            }

            return result;
        }

        private static AssignedInputDevice SelectPrimaryDevice(List<AssignedInputDevice> devices, AssignedInputDevice preferredDevice)
        {
            if (devices == null || devices.Count == 0)
            {
                return null;
            }

            if (preferredDevice != null)
            {
                AssignedInputDevice matchedDevice = devices.FirstOrDefault(device =>
                    device.Type == preferredDevice.Type &&
                    string.Equals(device.Id, preferredDevice.Id, StringComparison.Ordinal));

                if (matchedDevice != null)
                {
                    return matchedDevice;
                }
            }

            return devices[0];
        }

        private static AssignedInputDevice Clone(AssignedInputDevice device)
        {
            if (device == null)
            {
                return null;
            }

            return new AssignedInputDevice
            {
                Type = device.Type,
                Id = device.Id,
                ProfileName = device.ProfileName,
            };
        }
    }
}
