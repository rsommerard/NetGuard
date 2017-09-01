package eu.faircode.netguard;

/*
    This file is part of NetGuard.

    NetGuard is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NetGuard is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NetGuard.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2015-2017 by Marcel Bokhorst (M66B)
*/

public class Packet {
    public long time;
    public int version;
    public int protocol;
    public String flags;
    public String saddr;
    public int sport;
    public String daddr;
    public int dport;
    public String data;
    public int uid;

    public Packet() {
    }

    @Override
    public String toString() {
        String prtcl = protocol == 17 ? "UDP" : protocol == 6 ? "TCP" : "ICMP";
        String flgs = "".equals(flags) ? "_" : flags;
        String dt = "".equals(data) ? "_" : data;

        return "Packet {\"time\": " + time + ", \"version\": IP" + version +
                ", \"protocol\": " + prtcl + ", \"flags\": " + flgs +
                ", \"saddr\": " + saddr + ", \"sport\": " + sport +
                ", \"daddr\": " + daddr + ", \"dport\": " + dport +
                ", \"data\": " + dt + ", \"uid\": " + uid + " }";
    }
}
