ISO=/home/user/Doubao/chats/38442617987459330/dueslin-build/iso/DUESLIN-1.0.iso
DISK=/tmp/dtestC.qcow2
rm -f "$DISK" /tmp/serialF.log /tmp/qmonF.sock
qemu-img create -f qcow2 "$DISK" 20G 2>/dev/null
nohup qemu-system-x86_64 -m 2048 -smp 2 \
  -drive file="$DISK",if=virtio,format=qcow2 \
  -cdrom "$ISO" -boot d -vga std -display none \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial file:/tmp/serialF.log \
  -monitor unix:/tmp/qmonF.sock,server,nowait \
  -usb -device usb-tablet -no-reboot > /tmp/qemuF.log 2>&1 &
QEMUPID=$!
echo "QEMU=$QEMUPID"
sleep 12
python3 /home/user/Doubao/chats/38442617987459330/dueslin-project/test/qemu_keys.py /tmp/qmonF.sock down ret
echo "MENU_SELECTED"
sleep 20
grep -E "INSTALLER|TypeError|xinitrc" /tmp/serialF.log 2>/dev/null | tail -4
for i in $(seq 1 80); do
  sleep 15
  if grep -qE "安装完成|准备重启|重启系统" /tmp/serialF.log 2>/dev/null; then
    echo "INSTALL_DONE at $((i*15))s"; break
  fi
  if ! kill -0 $QEMUPID 2>/dev/null; then echo "QEMU_EXITED"; break; fi
done
grep -iE "选择整盘|正在解压|安装失败" /tmp/serialF.log 2>/dev/null | tail -4
echo "DISK_SIZE: $(ls -la $DISK 2>/dev/null | awk '{print $5}')"
pkill -9 -f qemu-system 2>/dev/null || true
echo "PHASE1_DONE"
